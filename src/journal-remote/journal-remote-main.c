/* SPDX-License-Identifier: LGPL-2.1+ */

#include <getopt.h>
#include <unistd.h>

#include "sd-daemon.h"

#include "conf-parser.h"
#include "def.h"
#include "fd-util.h"
#include "fileio.h"
#include "journal-remote-write.h"
#include "journal-remote.h"
#include "process-util.h"
#include "signal-util.h"
#include "socket-util.h"
#include "stat-util.h"
#include "string-table.h"
#include "strv.h"
#include "terminal-util.h"

#define PRIV_KEY_FILE CERTIFICATE_ROOT "/private/journal-remote.pem"
#define CERT_FILE     CERTIFICATE_ROOT "/certs/journal-remote.pem"
#define TRUST_FILE    CERTIFICATE_ROOT "/ca/trusted.pem"

static char* arg_url = NULL;
static char* arg_getter = NULL;
static char* arg_listen_raw = NULL;
static char* arg_listen_http = NULL;
static char* arg_listen_https = NULL;
static char** arg_files = NULL;
static int arg_compress = true;
static int arg_seal = false;
static int http_socket = -1, https_socket = -1;
static char** arg_gnutls_log = NULL;

static JournalWriteSplitMode arg_split_mode = _JOURNAL_WRITE_SPLIT_INVALID;
static char* arg_output = NULL;

static char *arg_key = NULL;
static char *arg_cert = NULL;
static char *arg_trust = NULL;
static bool arg_trust_all = false;

static const char* const journal_write_split_mode_table[_JOURNAL_WRITE_SPLIT_MAX] = {
        [JOURNAL_WRITE_SPLIT_NONE] = "none",
        [JOURNAL_WRITE_SPLIT_HOST] = "host",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP(journal_write_split_mode, JournalWriteSplitMode);
static DEFINE_CONFIG_PARSE_ENUM(config_parse_write_split_mode,
                                journal_write_split_mode,
                                JournalWriteSplitMode,
                                "Failed to parse split mode setting");

/**********************************************************************
 **********************************************************************
 **********************************************************************/

static int spawn_child(const char* child, char** argv) {
        pid_t child_pid;
        int fd[2], r;

        if (pipe(fd) < 0)
                return log_error_errno(errno, "Failed to create pager pipe: %m");

        r = safe_fork("(remote)", FORK_RESET_SIGNALS|FORK_DEATHSIG|FORK_LOG, &child_pid);
        if (r < 0) {
                safe_close_pair(fd);
                return r;
        }

        /* In the child */
        if (r == 0) {
                safe_close(fd[0]);

                r = rearrange_stdio(STDIN_FILENO, fd[1], STDERR_FILENO);
                if (r < 0) {
                        log_error_errno(r, "Failed to dup pipe to stdout: %m");
                        _exit(EXIT_FAILURE);
                }

                execvp(child, argv);
                log_error_errno(errno, "Failed to exec child %s: %m", child);
                _exit(EXIT_FAILURE);
        }

        safe_close(fd[1]);

        r = fd_nonblock(fd[0], true);
        if (r < 0)
                log_warning_errno(errno, "Failed to set child pipe to non-blocking: %m");

        return fd[0];
}

static int spawn_curl(const char* url) {
        char **argv = STRV_MAKE("curl",
                                "-HAccept: application/vnd.fdo.journal",
                                "--silent",
                                "--show-error",
                                url);
        int r;

        r = spawn_child("curl", argv);
        if (r < 0)
                log_error_errno(r, "Failed to spawn curl: %m");
        return r;
}

static int spawn_getter(const char *getter) {
        int r;
        _cleanup_strv_free_ char **words = NULL;

        assert(getter);
        r = strv_split_extract(&words, getter, WHITESPACE, EXTRACT_QUOTES);
        if (r < 0)
                return log_error_errno(r, "Failed to split getter option: %m");

        r = spawn_child(words[0], words);
        if (r < 0)
                log_error_errno(r, "Failed to spawn getter %s: %m", getter);

        return r;
}

/**********************************************************************
 **********************************************************************
 **********************************************************************/

static int null_timer_event_handler(sd_event_source *s,
                                uint64_t usec,
                                void *userdata);
static int dispatch_http_event(sd_event_source *event,
                               int fd,
                               uint32_t revents,
                               void *userdata);

static int request_meta(void **connection_cls, int fd, char *hostname) {
        RemoteSource *source;
        Writer *writer;
        int r;

        assert(connection_cls);
        if (*connection_cls)
                return 0;

        r = journal_remote_get_writer(journal_remote_server_global, hostname, &writer);
        if (r < 0)
                return log_warning_errno(r, "Failed to get writer for source %s: %m",
                                         hostname);

        source = source_new(fd, true, hostname, writer);
        if (!source) {
                writer_unref(writer);
                return log_oom();
        }

        log_debug("Added RemoteSource as connection metadata %p", source);

        *connection_cls = source;
        return 0;
}

static void request_meta_free(void *cls,
                              struct MHD_Connection *connection,
                              void **connection_cls,
                              enum MHD_RequestTerminationCode toe) {
        RemoteSource *s;

        assert(connection_cls);
        s = *connection_cls;

        if (s) {
                log_debug("Cleaning up connection metadata %p", s);
                source_free(s);
                *connection_cls = NULL;
        }
}

static int process_http_upload(
                struct MHD_Connection *connection,
                const char *upload_data,
                size_t *upload_data_size,
                RemoteSource *source) {

        bool finished = false;
        size_t remaining;
        int r;

        assert(source);

        log_trace("%s: connection %p, %zu bytes",
                  __func__, connection, *upload_data_size);

        if (*upload_data_size) {
                log_trace("Received %zu bytes", *upload_data_size);

                r = journal_importer_push_data(&source->importer,
                                               upload_data, *upload_data_size);
                if (r < 0)
                        return mhd_respond_oom(connection);

                *upload_data_size = 0;
        } else
                finished = true;

        for (;;) {
                r = process_source(source,
                                   journal_remote_server_global->compress,
                                   journal_remote_server_global->seal);
                if (r == -EAGAIN)
                        break;
                else if (r < 0) {
                        log_warning("Failed to process data for connection %p", connection);
                        if (r == -E2BIG)
                                return mhd_respondf(connection,
                                                    r, MHD_HTTP_PAYLOAD_TOO_LARGE,
                                                    "Entry is too large, maximum is " STRINGIFY(DATA_SIZE_MAX) " bytes.");
                        else
                                return mhd_respondf(connection,
                                                    r, MHD_HTTP_UNPROCESSABLE_ENTITY,
                                                    "Processing failed: %m.");
                }
        }

        if (!finished)
                return MHD_YES;

        /* The upload is finished */

        remaining = journal_importer_bytes_remaining(&source->importer);
        if (remaining > 0) {
                log_warning("Premature EOF byte. %zu bytes lost.", remaining);
                return mhd_respondf(connection,
                                    0, MHD_HTTP_EXPECTATION_FAILED,
                                    "Premature EOF. %zu bytes of trailing data not processed.",
                                    remaining);
        }

        return mhd_respond(connection, MHD_HTTP_ACCEPTED, "OK.");
};

static int request_handler(
                void *cls,
                struct MHD_Connection *connection,
                const char *url,
                const char *method,
                const char *version,
                const char *upload_data,
                size_t *upload_data_size,
                void **connection_cls) {

        const char *header;
        int r, code, fd;
        _cleanup_free_ char *hostname = NULL;

        assert(connection);
        assert(connection_cls);
        assert(url);
        assert(method);

        log_trace("Handling a connection %s %s %s", method, url, version);

        if (*connection_cls)
                return process_http_upload(connection,
                                           upload_data, upload_data_size,
                                           *connection_cls);

        if (!streq(method, "POST"))
                return mhd_respond(connection, MHD_HTTP_NOT_ACCEPTABLE, "Unsupported method.");

        if (!streq(url, "/upload"))
                return mhd_respond(connection, MHD_HTTP_NOT_FOUND, "Not found.");

        header = MHD_lookup_connection_value(connection,
                                             MHD_HEADER_KIND, "Content-Type");
        if (!header || !streq(header, "application/vnd.fdo.journal"))
                return mhd_respond(connection, MHD_HTTP_UNSUPPORTED_MEDIA_TYPE,
                                   "Content-Type: application/vnd.fdo.journal is required.");

        {
                const union MHD_ConnectionInfo *ci;

                ci = MHD_get_connection_info(connection,
                                             MHD_CONNECTION_INFO_CONNECTION_FD);
                if (!ci) {
                        log_error("MHD_get_connection_info failed: cannot get remote fd");
                        return mhd_respond(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                           "Cannot check remote address.");
                }

                fd = ci->connect_fd;
                assert(fd >= 0);
        }

        if (journal_remote_server_global->check_trust) {
                r = check_permissions(connection, &code, &hostname);
                if (r < 0)
                        return code;
        } else {
                r = getpeername_pretty(fd, false, &hostname);
                if (r < 0)
                        return mhd_respond(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                                           "Cannot check remote hostname.");
        }

        assert(hostname);

        r = request_meta(connection_cls, fd, hostname);
        if (r == -ENOMEM)
                return respond_oom(connection);
        else if (r < 0)
                return mhd_respondf(connection, r, MHD_HTTP_INTERNAL_SERVER_ERROR, "%m");

        hostname = NULL;
        return MHD_YES;
}

static int setup_microhttpd_server(RemoteServer *s,
                                   int fd,
                                   const char *key,
                                   const char *cert,
                                   const char *trust) {
        struct MHD_OptionItem opts[] = {
                { MHD_OPTION_NOTIFY_COMPLETED, (intptr_t) request_meta_free},
                { MHD_OPTION_EXTERNAL_LOGGER, (intptr_t) microhttpd_logger},
                { MHD_OPTION_LISTEN_SOCKET, fd},
                { MHD_OPTION_CONNECTION_MEMORY_LIMIT, 128*1024},
                { MHD_OPTION_END},
                { MHD_OPTION_END},
                { MHD_OPTION_END},
                { MHD_OPTION_END},
                { MHD_OPTION_END}};
        int opts_pos = 4;
        int flags =
                MHD_USE_DEBUG |
                MHD_USE_DUAL_STACK |
                MHD_USE_EPOLL |
                MHD_USE_ITC;

        const union MHD_DaemonInfo *info;
        int r, epoll_fd;
        MHDDaemonWrapper *d;

        assert(fd >= 0);

        r = fd_nonblock(fd, true);
        if (r < 0)
                return log_error_errno(r, "Failed to make fd:%d nonblocking: %m", fd);

/* MHD_OPTION_STRICT_FOR_CLIENT is introduced in microhttpd 0.9.54,
 * and MHD_USE_PEDANTIC_CHECKS will be deprecated in future.
 * If MHD_USE_PEDANTIC_CHECKS is '#define'd, then it is deprecated
 * and we should use MHD_OPTION_STRICT_FOR_CLIENT. On the other hand,
 * if MHD_USE_PEDANTIC_CHECKS is not '#define'd, then it is not
 * deprecated yet and there exists an enum element with the same name.
 * So we can safely use it. */
#ifdef MHD_USE_PEDANTIC_CHECKS
        opts[opts_pos++] = (struct MHD_OptionItem)
                {MHD_OPTION_STRICT_FOR_CLIENT, 1};
#else
        flags |= MHD_USE_PEDANTIC_CHECKS;
#endif

        if (key) {
                assert(cert);

                opts[opts_pos++] = (struct MHD_OptionItem)
                        {MHD_OPTION_HTTPS_MEM_KEY, 0, (char*) key};
                opts[opts_pos++] = (struct MHD_OptionItem)
                        {MHD_OPTION_HTTPS_MEM_CERT, 0, (char*) cert};

                flags |= MHD_USE_TLS;

                if (trust)
                        opts[opts_pos++] = (struct MHD_OptionItem)
                                {MHD_OPTION_HTTPS_MEM_TRUST, 0, (char*) trust};
        }

        d = new(MHDDaemonWrapper, 1);
        if (!d)
                return log_oom();

        d->fd = (uint64_t) fd;

        d->daemon = MHD_start_daemon(flags, 0,
                                     NULL, NULL,
                                     request_handler, NULL,
                                     MHD_OPTION_ARRAY, opts,
                                     MHD_OPTION_END);
        if (!d->daemon) {
                log_error("Failed to start µhttp daemon");
                r = -EINVAL;
                goto error;
        }

        log_debug("Started MHD %s daemon on fd:%d (wrapper @ %p)",
                  key ? "HTTPS" : "HTTP", fd, d);

        info = MHD_get_daemon_info(d->daemon, MHD_DAEMON_INFO_EPOLL_FD_LINUX_ONLY);
        if (!info) {
                log_error("µhttp returned NULL daemon info");
                r = -EOPNOTSUPP;
                goto error;
        }

        epoll_fd = info->listen_fd;
        if (epoll_fd < 0) {
                log_error("µhttp epoll fd is invalid");
                r = -EUCLEAN;
                goto error;
        }

        r = sd_event_add_io(s->events, &d->io_event,
                            epoll_fd, EPOLLIN,
                            dispatch_http_event, d);
        if (r < 0) {
                log_error_errno(r, "Failed to add event callback: %m");
                goto error;
        }

        r = sd_event_source_set_description(d->io_event, "io_event");
        if (r < 0) {
                log_error_errno(r, "Failed to set source name: %m");
                goto error;
        }

        r = sd_event_add_time(s->events, &d->timer_event,
                              CLOCK_MONOTONIC, (uint64_t) -1, 0,
                              null_timer_event_handler, d);
        if (r < 0) {
                log_error_errno(r, "Failed to add timer_event: %m");
                goto error;
        }

        r = sd_event_source_set_description(d->timer_event, "timer_event");
        if (r < 0) {
                log_error_errno(r, "Failed to set source name: %m");
                goto error;
        }

        r = hashmap_ensure_allocated(&s->daemons, &uint64_hash_ops);
        if (r < 0) {
                log_oom();
                goto error;
        }

        r = hashmap_put(s->daemons, &d->fd, d);
        if (r < 0) {
                log_error_errno(r, "Failed to add daemon to hashmap: %m");
                goto error;
        }

        s->active++;
        return 0;

error:
        MHD_stop_daemon(d->daemon);
        free(d->daemon);
        free(d);
        return r;
}

static int setup_microhttpd_socket(RemoteServer *s,
                                   const char *address,
                                   const char *key,
                                   const char *cert,
                                   const char *trust) {
        int fd;

        fd = make_socket_fd(LOG_DEBUG, address, SOCK_STREAM, SOCK_CLOEXEC);
        if (fd < 0)
                return fd;

        return setup_microhttpd_server(s, fd, key, cert, trust);
}

static int null_timer_event_handler(sd_event_source *timer_event,
                                    uint64_t usec,
                                    void *userdata) {
        return dispatch_http_event(timer_event, 0, 0, userdata);
}

static int dispatch_http_event(sd_event_source *event,
                               int fd,
                               uint32_t revents,
                               void *userdata) {
        MHDDaemonWrapper *d = userdata;
        int r;
        MHD_UNSIGNED_LONG_LONG timeout = ULONG_LONG_MAX;

        assert(d);

        r = MHD_run(d->daemon);
        if (r == MHD_NO) {
                log_error("MHD_run failed!");
                // XXX: unregister daemon
                return -EINVAL;
        }
        if (MHD_get_timeout(d->daemon, &timeout) == MHD_NO)
                timeout = ULONG_LONG_MAX;

        r = sd_event_source_set_time(d->timer_event, timeout);
        if (r < 0) {
                log_warning_errno(r, "Unable to set event loop timeout: %m, this may result in indefinite blocking!");
                return 1;
        }

        r = sd_event_source_set_enabled(d->timer_event, SD_EVENT_ON);
        if (r < 0)
                log_warning_errno(r, "Unable to enable timer_event: %m, this may result in indefinite blocking!");

        return 1; /* work to do */
}

/**********************************************************************
 **********************************************************************
 **********************************************************************/

static int setup_signals(RemoteServer *s) {
        int r;

        assert(s);

        assert_se(sigprocmask_many(SIG_SETMASK, NULL, SIGINT, SIGTERM, -1) >= 0);

        r = sd_event_add_signal(s->events, &s->sigterm_event, SIGTERM, NULL, s);
        if (r < 0)
                return r;

        r = sd_event_add_signal(s->events, &s->sigint_event, SIGINT, NULL, s);
        if (r < 0)
                return r;

        return 0;
}

static int setup_raw_socket(RemoteServer *s, const char *address) {
        int fd;

        fd = make_socket_fd(LOG_INFO, address, SOCK_STREAM, SOCK_CLOEXEC);
        if (fd < 0)
                return fd;

        return journal_remote_add_raw_socket(s, fd);
}

static int create_remoteserver(
                RemoteServer *s,
                const char* key,
                const char* cert,
                const char* trust) {

        int r, n, fd;
        char **file;

        r = journal_remote_server_init(s, arg_output, arg_split_mode, arg_compress, arg_seal);
        if (r < 0)
                return r;

        setup_signals(s);

        n = sd_listen_fds(true);
        if (n < 0)
                return log_error_errno(n, "Failed to read listening file descriptors from environment: %m");
        else
                log_debug("Received %d descriptors", n);

        if (MAX(http_socket, https_socket) >= SD_LISTEN_FDS_START + n) {
                log_error("Received fewer sockets than expected");
                return -EBADFD;
        }

        for (fd = SD_LISTEN_FDS_START; fd < SD_LISTEN_FDS_START + n; fd++) {
                if (sd_is_socket(fd, AF_UNSPEC, 0, true)) {
                        log_debug("Received a listening socket (fd:%d)", fd);

                        if (fd == http_socket)
                                r = setup_microhttpd_server(s, fd, NULL, NULL, NULL);
                        else if (fd == https_socket)
                                r = setup_microhttpd_server(s, fd, key, cert, trust);
                        else
                                r = journal_remote_add_raw_socket(s, fd);
                } else if (sd_is_socket(fd, AF_UNSPEC, 0, false)) {
                        char *hostname;

                        r = getpeername_pretty(fd, false, &hostname);
                        if (r < 0)
                                return log_error_errno(r, "Failed to retrieve remote name: %m");

                        log_debug("Received a connection socket (fd:%d) from %s", fd, hostname);

                        r = journal_remote_add_source(s, fd, hostname, true);
                } else {
                        log_error("Unknown socket passed on fd:%d", fd);

                        return -EINVAL;
                }

                if (r < 0)
                        return log_error_errno(r, "Failed to register socket (fd:%d): %m",
                                               fd);
        }

        if (arg_getter) {
                log_info("Spawning getter %s...", arg_getter);
                fd = spawn_getter(arg_getter);
                if (fd < 0)
                        return fd;

                r = journal_remote_add_source(s, fd, (char*) arg_output, false);
                if (r < 0)
                        return r;
        }

        if (arg_url) {
                const char *url;
                char *hostname, *p;

                if (!strstr(arg_url, "/entries")) {
                        if (endswith(arg_url, "/"))
                                url = strjoina(arg_url, "entries");
                        else
                                url = strjoina(arg_url, "/entries");
                }
                else
                        url = strdupa(arg_url);

                log_info("Spawning curl %s...", url);
                fd = spawn_curl(url);
                if (fd < 0)
                        return fd;

                hostname =
                        startswith(arg_url, "https://") ?:
                        startswith(arg_url, "http://") ?:
                        arg_url;

                hostname = strdupa(hostname);
                if ((p = strchr(hostname, '/')))
                        *p = '\0';
                if ((p = strchr(hostname, ':')))
                        *p = '\0';

                r = journal_remote_add_source(s, fd, hostname, false);
                if (r < 0)
                        return r;
        }

        if (arg_listen_raw) {
                log_debug("Listening on a socket...");
                r = setup_raw_socket(s, arg_listen_raw);
                if (r < 0)
                        return r;
        }

        if (arg_listen_http) {
                r = setup_microhttpd_socket(s, arg_listen_http, NULL, NULL, NULL);
                if (r < 0)
                        return r;
        }

        if (arg_listen_https) {
                r = setup_microhttpd_socket(s, arg_listen_https, key, cert, trust);
                if (r < 0)
                        return r;
        }

        STRV_FOREACH(file, arg_files) {
                const char *output_name;

                if (streq(*file, "-")) {
                        log_debug("Using standard input as source.");

                        fd = STDIN_FILENO;
                        output_name = "stdin";
                } else {
                        log_debug("Reading file %s...", *file);

                        fd = open(*file, O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NONBLOCK);
                        if (fd < 0)
                                return log_error_errno(errno, "Failed to open %s: %m", *file);
                        output_name = *file;
                }

                r = journal_remote_add_source(s, fd, (char*) output_name, false);
                if (r < 0)
                        return r;
        }

        if (s->active == 0) {
                log_error("Zero sources specified");
                return -EINVAL;
        }

        if (arg_split_mode == JOURNAL_WRITE_SPLIT_NONE) {
                /* In this case we know what the writer will be
                   called, so we can create it and verify that we can
                   create output as expected. */
                r = journal_remote_get_writer(s, NULL, &s->_single_writer);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int negative_fd(const char *spec) {
        /* Return a non-positive number as its inverse, -EINVAL otherwise. */

        int fd, r;

        r = safe_atoi(spec, &fd);
        if (r < 0)
                return r;

        if (fd > 0)
                return -EINVAL;
        else
                return -fd;
}

static int parse_config(void) {
        const ConfigTableItem items[] = {
                { "Remote",  "Seal",                   config_parse_bool,             0, &arg_seal       },
                { "Remote",  "SplitMode",              config_parse_write_split_mode, 0, &arg_split_mode },
                { "Remote",  "ServerKeyFile",          config_parse_path,             0, &arg_key        },
                { "Remote",  "ServerCertificateFile",  config_parse_path,             0, &arg_cert       },
                { "Remote",  "TrustedCertificateFile", config_parse_path,             0, &arg_trust      },
                {}
        };

        return config_parse_many_nulstr(PKGSYSCONFDIR "/journal-remote.conf",
                                        CONF_PATHS_NULSTR("systemd/journal-remote.conf.d"),
                                        "Remote\0", config_item_table_lookup, items,
                                        CONFIG_PARSE_WARN, NULL);
}

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-journal-remote.service", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...] {FILE|-}...\n\n"
               "Write external journal events to journal file(s).\n\n"
               "  -h --help                 Show this help\n"
               "     --version              Show package version\n"
               "     --url=URL              Read events from systemd-journal-gatewayd at URL\n"
               "     --getter=COMMAND       Read events from the output of COMMAND\n"
               "     --listen-raw=ADDR      Listen for connections at ADDR\n"
               "     --listen-http=ADDR     Listen for HTTP connections at ADDR\n"
               "     --listen-https=ADDR    Listen for HTTPS connections at ADDR\n"
               "  -o --output=FILE|DIR      Write output to FILE or DIR/external-*.journal\n"
               "     --compress[=BOOL]      XZ-compress the output journal (default: yes)\n"
               "     --seal[=BOOL]          Use event sealing (default: no)\n"
               "     --key=FILENAME         SSL key in PEM format (default:\n"
               "                            \"" PRIV_KEY_FILE "\")\n"
               "     --cert=FILENAME        SSL certificate in PEM format (default:\n"
               "                            \"" CERT_FILE "\")\n"
               "     --trust=FILENAME|all   SSL CA certificate or disable checking (default:\n"
               "                            \"" TRUST_FILE "\")\n"
               "     --gnutls-log=CATEGORY...\n"
               "                            Specify a list of gnutls logging categories\n"
               "     --split-mode=none|host How many output files to create\n"
               "\nNote: file descriptors from sd_listen_fds() will be consumed, too.\n"
               "\nSee the %s for details.\n"
               , program_invocation_short_name
               , link
        );

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_URL,
                ARG_LISTEN_RAW,
                ARG_LISTEN_HTTP,
                ARG_LISTEN_HTTPS,
                ARG_GETTER,
                ARG_SPLIT_MODE,
                ARG_COMPRESS,
                ARG_SEAL,
                ARG_KEY,
                ARG_CERT,
                ARG_TRUST,
                ARG_GNUTLS_LOG,
        };

        static const struct option options[] = {
                { "help",         no_argument,       NULL, 'h'              },
                { "version",      no_argument,       NULL, ARG_VERSION      },
                { "url",          required_argument, NULL, ARG_URL          },
                { "getter",       required_argument, NULL, ARG_GETTER       },
                { "listen-raw",   required_argument, NULL, ARG_LISTEN_RAW   },
                { "listen-http",  required_argument, NULL, ARG_LISTEN_HTTP  },
                { "listen-https", required_argument, NULL, ARG_LISTEN_HTTPS },
                { "output",       required_argument, NULL, 'o'              },
                { "split-mode",   required_argument, NULL, ARG_SPLIT_MODE   },
                { "compress",     optional_argument, NULL, ARG_COMPRESS     },
                { "seal",         optional_argument, NULL, ARG_SEAL         },
                { "key",          required_argument, NULL, ARG_KEY          },
                { "cert",         required_argument, NULL, ARG_CERT         },
                { "trust",        required_argument, NULL, ARG_TRUST        },
                { "gnutls-log",   required_argument, NULL, ARG_GNUTLS_LOG   },
                {}
        };

        int c, r;
        bool type_a, type_b;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "ho:", options, NULL)) >= 0)
                switch(c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case ARG_URL:
                        if (arg_url) {
                                log_error("cannot currently set more than one --url");
                                return -EINVAL;
                        }

                        arg_url = optarg;
                        break;

                case ARG_GETTER:
                        if (arg_getter) {
                                log_error("cannot currently use --getter more than once");
                                return -EINVAL;
                        }

                        arg_getter = optarg;
                        break;

                case ARG_LISTEN_RAW:
                        if (arg_listen_raw) {
                                log_error("cannot currently use --listen-raw more than once");
                                return -EINVAL;
                        }

                        arg_listen_raw = optarg;
                        break;

                case ARG_LISTEN_HTTP:
                        if (arg_listen_http || http_socket >= 0) {
                                log_error("cannot currently use --listen-http more than once");
                                return -EINVAL;
                        }

                        r = negative_fd(optarg);
                        if (r >= 0)
                                http_socket = r;
                        else
                                arg_listen_http = optarg;
                        break;

                case ARG_LISTEN_HTTPS:
                        if (arg_listen_https || https_socket >= 0) {
                                log_error("cannot currently use --listen-https more than once");
                                return -EINVAL;
                        }

                        r = negative_fd(optarg);
                        if (r >= 0)
                                https_socket = r;
                        else
                                arg_listen_https = optarg;

                        break;

                case ARG_KEY:
                        if (arg_key) {
                                log_error("Key file specified twice");
                                return -EINVAL;
                        }

                        arg_key = strdup(optarg);
                        if (!arg_key)
                                return log_oom();

                        break;

                case ARG_CERT:
                        if (arg_cert) {
                                log_error("Certificate file specified twice");
                                return -EINVAL;
                        }

                        arg_cert = strdup(optarg);
                        if (!arg_cert)
                                return log_oom();

                        break;

                case ARG_TRUST:
                        if (arg_trust || arg_trust_all) {
                                log_error("Confusing trusted CA configuration");
                                return -EINVAL;
                        }

                        if (streq(optarg, "all"))
                                arg_trust_all = true;
                        else {
#if HAVE_GNUTLS
                                arg_trust = strdup(optarg);
                                if (!arg_trust)
                                        return log_oom();
#else
                                log_error("Option --trust is not available.");
                                return -EINVAL;
#endif
                        }

                        break;

                case 'o':
                        if (arg_output) {
                                log_error("cannot use --output/-o more than once");
                                return -EINVAL;
                        }

                        arg_output = optarg;
                        break;

                case ARG_SPLIT_MODE:
                        arg_split_mode = journal_write_split_mode_from_string(optarg);
                        if (arg_split_mode == _JOURNAL_WRITE_SPLIT_INVALID) {
                                log_error("Invalid split mode: %s", optarg);
                                return -EINVAL;
                        }
                        break;

                case ARG_COMPRESS:
                        if (optarg) {
                                r = parse_boolean(optarg);
                                if (r < 0) {
                                        log_error("Failed to parse --compress= parameter.");
                                        return -EINVAL;
                                }

                                arg_compress = !!r;
                        } else
                                arg_compress = true;

                        break;

                case ARG_SEAL:
                        if (optarg) {
                                r = parse_boolean(optarg);
                                if (r < 0) {
                                        log_error("Failed to parse --seal= parameter.");
                                        return -EINVAL;
                                }

                                arg_seal = !!r;
                        } else
                                arg_seal = true;

                        break;

                case ARG_GNUTLS_LOG: {
#if HAVE_GNUTLS
                        const char* p = optarg;
                        for (;;) {
                                _cleanup_free_ char *word = NULL;

                                r = extract_first_word(&p, &word, ",", 0);
                                if (r < 0)
                                        return log_error_errno(r, "Failed to parse --gnutls-log= argument: %m");

                                if (r == 0)
                                        break;

                                if (strv_push(&arg_gnutls_log, word) < 0)
                                        return log_oom();

                                word = NULL;
                        }
                        break;
#else
                        log_error("Option --gnutls-log is not available.");
                        return -EINVAL;
#endif
                }

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unknown option code.");
                }

        if (optind < argc)
                arg_files = argv + optind;

        type_a = arg_getter || !strv_isempty(arg_files);
        type_b = arg_url
                || arg_listen_raw
                || arg_listen_http || arg_listen_https
                || sd_listen_fds(false) > 0;
        if (type_a && type_b) {
                log_error("Cannot use file input or --getter with "
                          "--arg-listen-... or socket activation.");
                return -EINVAL;
        }
        if (type_a) {
                if (!arg_output) {
                        log_error("Option --output must be specified with file input or --getter.");
                        return -EINVAL;
                }

                if (!IN_SET(arg_split_mode, JOURNAL_WRITE_SPLIT_NONE, _JOURNAL_WRITE_SPLIT_INVALID)) {
                        log_error("For active sources, only --split-mode=none is allowed.");
                        return -EINVAL;
                }

                arg_split_mode = JOURNAL_WRITE_SPLIT_NONE;
        }

        if (arg_split_mode == _JOURNAL_WRITE_SPLIT_INVALID)
                arg_split_mode = JOURNAL_WRITE_SPLIT_HOST;

        if (arg_split_mode == JOURNAL_WRITE_SPLIT_NONE && arg_output) {
                if (is_dir(arg_output, true) > 0) {
                        log_error("For SplitMode=none, output must be a file.");
                        return -EINVAL;
                }
                if (!endswith(arg_output, ".journal")) {
                        log_error("For SplitMode=none, output file name must end with .journal.");
                        return -EINVAL;
                }
        }

        if (arg_split_mode == JOURNAL_WRITE_SPLIT_HOST
            && arg_output && is_dir(arg_output, true) <= 0) {
                log_error("For SplitMode=host, output must be a directory.");
                return -EINVAL;
        }

        log_debug("Full config: SplitMode=%s Key=%s Cert=%s Trust=%s",
                  journal_write_split_mode_to_string(arg_split_mode),
                  strna(arg_key),
                  strna(arg_cert),
                  strna(arg_trust));

        return 1 /* work to do */;
}

static int load_certificates(char **key, char **cert, char **trust) {
        int r;

        r = read_full_file(arg_key ?: PRIV_KEY_FILE, key, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to read key from file '%s': %m",
                                       arg_key ?: PRIV_KEY_FILE);

        r = read_full_file(arg_cert ?: CERT_FILE, cert, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to read certificate from file '%s': %m",
                                       arg_cert ?: CERT_FILE);

        if (arg_trust_all)
                log_info("Certificate checking disabled.");
        else {
                r = read_full_file(arg_trust ?: TRUST_FILE, trust, NULL);
                if (r < 0)
                        return log_error_errno(r, "Failed to read CA certificate file '%s': %m",
                                               arg_trust ?: TRUST_FILE);
        }

        if ((arg_listen_raw || arg_listen_http) && *trust) {
                log_error("Option --trust makes all non-HTTPS connections untrusted.");
                return -EINVAL;
        }

        return 0;
}

int main(int argc, char **argv) {
        RemoteServer s = {};
        int r;
        _cleanup_free_ char *key = NULL, *cert = NULL, *trust = NULL;

        log_show_color(true);
        log_parse_environment();

        r = parse_config();
        if (r < 0)
                return EXIT_FAILURE;

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

        if (arg_listen_http || arg_listen_https) {
                r = setup_gnutls_logger(arg_gnutls_log);
                if (r < 0)
                        return EXIT_FAILURE;
        }

        if (arg_listen_https || https_socket >= 0)
                if (load_certificates(&key, &cert, &trust) < 0)
                        return EXIT_FAILURE;

        if (create_remoteserver(&s, key, cert, trust) < 0)
                return EXIT_FAILURE;

        r = sd_event_set_watchdog(s.events, true);
        if (r < 0)
                log_error_errno(r, "Failed to enable watchdog: %m");
        else
                log_debug("Watchdog is %sd.", enable_disable(r > 0));

        log_debug("%s running as pid "PID_FMT,
                  program_invocation_short_name, getpid_cached());
        sd_notify(false,
                  "READY=1\n"
                  "STATUS=Processing requests...");

        while (s.active) {
                r = sd_event_get_state(s.events);
                if (r < 0)
                        break;
                if (r == SD_EVENT_FINISHED)
                        break;

                r = sd_event_run(s.events, -1);
                if (r < 0) {
                        log_error_errno(r, "Failed to run event loop: %m");
                        break;
                }
        }

        sd_notifyf(false,
                   "STOPPING=1\n"
                   "STATUS=Shutting down after writing %" PRIu64 " entries...", s.event_count);
        log_info("Finishing after writing %" PRIu64 " entries", s.event_count);

        journal_remote_server_destroy(&s);

        free(arg_key);
        free(arg_cert);
        free(arg_trust);

        return r >= 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
