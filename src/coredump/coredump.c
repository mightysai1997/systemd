/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/xattr.h>
#include <unistd.h>

#if HAVE_ELFUTILS
#include <dwarf.h>
#include <elfutils/libdwfl.h>
#endif

#include "sd-daemon.h"
#include "sd-journal.h"
#include "sd-login.h"
#include "sd-messages.h"

#include "acl-util.h"
#include "alloc-util.h"
#include "capability-util.h"
#include "cgroup-util.h"
#include "compress.h"
#include "conf-parser.h"
#include "copy.h"
#include "coredump-vacuum.h"
#include "dirent-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "io-util.h"
#include "journal-importer.h"
#include "log.h"
#include "macro.h"
#include "main-func.h"
#include "memory-util.h"
#include "mkdir.h"
#include "parse-util.h"
#include "process-util.h"
#include "signal-util.h"
#include "socket-util.h"
#include "special.h"
#include "stacktrace.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "tmpfile-util.h"
#include "user-record.h"
#include "user-util.h"

/* The maximum size up to which we process coredumps */
#define PROCESS_SIZE_MAX ((uint64_t) (2LLU*1024LLU*1024LLU*1024LLU))

/* The maximum size up to which we leave the coredump around on disk */
#define EXTERNAL_SIZE_MAX PROCESS_SIZE_MAX

/* The maximum size up to which we store the coredump in the journal */
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#define JOURNAL_SIZE_MAX ((size_t) (767LU*1024LU*1024LU))
#else
/* oss-fuzz limits memory usage. */
#define JOURNAL_SIZE_MAX ((size_t) (10LU*1024LU*1024LU))
#endif

/* Make sure to not make this larger than the maximum journal entry
 * size. See DATA_SIZE_MAX in journal-importer.h. */
assert_cc(JOURNAL_SIZE_MAX <= DATA_SIZE_MAX);

enum {
        /* We use these as array indexes for our process metadata cache.
         *
         * The first indices of the cache stores the same metadata as the ones passed by
         * the kernel via argv[], ie the strings array passed by the kernel according to
         * our pattern defined in /proc/sys/kernel/core_pattern (see man:core(5)). */

        META_ARGV_PID,          /* %P: as seen in the initial pid namespace */
        META_ARGV_UID,          /* %u: as seen in the initial user namespace */
        META_ARGV_GID,          /* %g: as seen in the initial user namespace */
        META_ARGV_SIGNAL,       /* %s: number of signal causing dump */
        META_ARGV_TIMESTAMP,    /* %t: time of dump, expressed as seconds since the Epoch (we expand this to µs granularity) */
        META_ARGV_RLIMIT,       /* %c: core file size soft resource limit */
        META_ARGV_HOSTNAME,     /* %h: hostname */
        _META_ARGV_MAX,

        /* The following indexes are cached for a couple of special fields we use (and
         * thereby need to be retrieved quickly) for naming coredump files, and attaching
         * xattrs. Unlike the previous ones they are retrieved from the runtime
         * environment. */

        META_COMM = _META_ARGV_MAX,
        _META_MANDATORY_MAX,

        /* The rest are similar to the previous ones except that we won't fail if one of
         * them is missing. */

        META_EXE = _META_MANDATORY_MAX,
        META_UNIT,
        _META_MAX
};

static const char * const meta_field_names[_META_MAX] = {
        [META_ARGV_PID]          = "COREDUMP_PID=",
        [META_ARGV_UID]          = "COREDUMP_UID=",
        [META_ARGV_GID]          = "COREDUMP_GID=",
        [META_ARGV_SIGNAL]       = "COREDUMP_SIGNAL=",
        [META_ARGV_TIMESTAMP]    = "COREDUMP_TIMESTAMP=",
        [META_ARGV_RLIMIT]       = "COREDUMP_RLIMIT=",
        [META_ARGV_HOSTNAME]     = "COREDUMP_HOSTNAME=",
        [META_COMM]              = "COREDUMP_COMM=",
        [META_EXE]               = "COREDUMP_EXE=",
        [META_UNIT]              = "COREDUMP_UNIT=",
};

typedef struct Context {
        const char *meta[_META_MAX];
        pid_t pid;
        bool is_pid1;
        bool is_journald;
} Context;

typedef enum CoredumpStorage {
        COREDUMP_STORAGE_NONE,
        COREDUMP_STORAGE_EXTERNAL,
        COREDUMP_STORAGE_JOURNAL,
        _COREDUMP_STORAGE_MAX,
        _COREDUMP_STORAGE_INVALID = -EINVAL,
} CoredumpStorage;

static const char* const coredump_storage_table[_COREDUMP_STORAGE_MAX] = {
        [COREDUMP_STORAGE_NONE] = "none",
        [COREDUMP_STORAGE_EXTERNAL] = "external",
        [COREDUMP_STORAGE_JOURNAL] = "journal",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP(coredump_storage, CoredumpStorage);
static DEFINE_CONFIG_PARSE_ENUM(config_parse_coredump_storage, coredump_storage, CoredumpStorage, "Failed to parse storage setting");

static CoredumpStorage arg_storage = COREDUMP_STORAGE_EXTERNAL;
static bool arg_compress = true;
static uint64_t arg_process_size_max = PROCESS_SIZE_MAX;
static uint64_t arg_external_size_max = EXTERNAL_SIZE_MAX;
static uint64_t arg_journal_size_max = JOURNAL_SIZE_MAX;
static uint64_t arg_keep_free = UINT64_MAX;
static uint64_t arg_max_use = UINT64_MAX;

static int parse_config(void) {
        static const ConfigTableItem items[] = {
                { "Coredump", "Storage",          config_parse_coredump_storage,  0, &arg_storage           },
                { "Coredump", "Compress",         config_parse_bool,              0, &arg_compress          },
                { "Coredump", "ProcessSizeMax",   config_parse_iec_uint64,        0, &arg_process_size_max  },
                { "Coredump", "ExternalSizeMax",  config_parse_iec_uint64,        0, &arg_external_size_max },
                { "Coredump", "JournalSizeMax",   config_parse_iec_size,          0, &arg_journal_size_max  },
                { "Coredump", "KeepFree",         config_parse_iec_uint64,        0, &arg_keep_free         },
                { "Coredump", "MaxUse",           config_parse_iec_uint64,        0, &arg_max_use           },
                {}
        };

        return config_parse_many_nulstr(
                        PKGSYSCONFDIR "/coredump.conf",
                        CONF_PATHS_NULSTR("systemd/coredump.conf.d"),
                        "Coredump\0",
                        config_item_table_lookup, items,
                        CONFIG_PARSE_WARN,
                        NULL,
                        NULL);
}

static uint64_t storage_size_max(void) {
        if (arg_storage == COREDUMP_STORAGE_EXTERNAL)
                return arg_external_size_max;
        if (arg_storage == COREDUMP_STORAGE_JOURNAL)
                return arg_journal_size_max;
        assert(arg_storage == COREDUMP_STORAGE_NONE);
        return 0;
}

static int fix_acl(int fd, uid_t uid) {

#if HAVE_ACL
        int r;

        assert(fd >= 0);
        assert(uid_is_valid(uid));

        if (uid_is_system(uid) || uid_is_dynamic(uid) || uid == UID_NOBODY)
                return 0;

        /* Make sure normal users can read (but not write or delete) their own coredumps */
        r = fd_add_uid_acl_permission(fd, uid, ACL_READ);
        if (r < 0)
                return log_error_errno(r, "Failed to adjust ACL of the coredump: %m");
#endif

        return 0;
}

static int fix_xattr(int fd, const Context *context) {

        static const char * const xattrs[_META_MAX] = {
                [META_ARGV_PID]          = "user.coredump.pid",
                [META_ARGV_UID]          = "user.coredump.uid",
                [META_ARGV_GID]          = "user.coredump.gid",
                [META_ARGV_SIGNAL]       = "user.coredump.signal",
                [META_ARGV_TIMESTAMP]    = "user.coredump.timestamp",
                [META_ARGV_RLIMIT]       = "user.coredump.rlimit",
                [META_ARGV_HOSTNAME]     = "user.coredump.hostname",
                [META_COMM]              = "user.coredump.comm",
                [META_EXE]               = "user.coredump.exe",
        };

        int r = 0;

        assert(fd >= 0);

        /* Attach some metadata to coredumps via extended
         * attributes. Just because we can. */

        for (unsigned i = 0; i < _META_MAX; i++) {
                int k;

                if (isempty(context->meta[i]) || !xattrs[i])
                        continue;

                k = fsetxattr(fd, xattrs[i], context->meta[i], strlen(context->meta[i]), XATTR_CREATE);
                if (k < 0 && r == 0)
                        r = -errno;
        }

        return r;
}

#define filename_escape(s) xescape((s), "./ ")

static const char *coredump_tmpfile_name(const char *s) {
        return s ? s : "(unnamed temporary file)";
}

static int fix_permissions(
                int fd,
                const char *filename,
                const char *target,
                const Context *context,
                uid_t uid) {

        int r;

        assert(fd >= 0);
        assert(target);
        assert(context);

        /* Ignore errors on these */
        (void) fchmod(fd, 0640);
        (void) fix_acl(fd, uid);
        (void) fix_xattr(fd, context);

        if (fsync(fd) < 0)
                return log_error_errno(errno, "Failed to sync coredump %s: %m", coredump_tmpfile_name(filename));

        (void) fsync_directory_of_file(fd);

        r = link_tmpfile(fd, filename, target);
        if (r < 0)
                return log_error_errno(r, "Failed to move coredump %s into place: %m", target);

        return 0;
}

static int maybe_remove_external_coredump(const char *filename, uint64_t size) {

        /* Returns 1 if might remove, 0 if will not remove, < 0 on error. */

        if (arg_storage == COREDUMP_STORAGE_EXTERNAL &&
            size <= arg_external_size_max)
                return 0;

        if (!filename)
                return 1;

        if (unlink(filename) < 0 && errno != ENOENT)
                return log_error_errno(errno, "Failed to unlink %s: %m", filename);

        return 1;
}

static int make_filename(const Context *context, char **ret) {
        _cleanup_free_ char *c = NULL, *u = NULL, *p = NULL, *t = NULL;
        sd_id128_t boot = {};
        int r;

        assert(context);

        c = filename_escape(context->meta[META_COMM]);
        if (!c)
                return -ENOMEM;

        u = filename_escape(context->meta[META_ARGV_UID]);
        if (!u)
                return -ENOMEM;

        r = sd_id128_get_boot(&boot);
        if (r < 0)
                return r;

        p = filename_escape(context->meta[META_ARGV_PID]);
        if (!p)
                return -ENOMEM;

        t = filename_escape(context->meta[META_ARGV_TIMESTAMP]);
        if (!t)
                return -ENOMEM;

        if (asprintf(ret,
                     "/var/lib/systemd/coredump/core.%s.%s." SD_ID128_FORMAT_STR ".%s.%s",
                     c,
                     u,
                     SD_ID128_FORMAT_VAL(boot),
                     p,
                     t) < 0)
                return -ENOMEM;

        return 0;
}

static int save_external_coredump(
                const Context *context,
                int input_fd,
                char **ret_filename,
                int *ret_node_fd,
                int *ret_data_fd,
                uint64_t *ret_size,
                uint64_t *ret_compressed_size,
                bool *ret_truncated) {

        _cleanup_free_ char *fn = NULL, *tmp = NULL;
        _cleanup_close_ int fd = -1;
        uint64_t rlimit, process_limit, max_size;
        struct stat st;
        uid_t uid;
        int r;

        assert(context);
        assert(ret_filename);
        assert(ret_node_fd);
        assert(ret_data_fd);
        assert(ret_size);

        r = parse_uid(context->meta[META_ARGV_UID], &uid);
        if (r < 0)
                return log_error_errno(r, "Failed to parse UID: %m");

        r = safe_atou64(context->meta[META_ARGV_RLIMIT], &rlimit);
        if (r < 0)
                return log_error_errno(r, "Failed to parse resource limit '%s': %m",
                                       context->meta[META_ARGV_RLIMIT]);
        if (rlimit < page_size())
                /* Is coredumping disabled? Then don't bother saving/processing the
                 * coredump. Anything below PAGE_SIZE cannot give a readable coredump
                 * (the kernel uses ELF_EXEC_PAGESIZE which is not easily accessible, but
                 * is usually the same as PAGE_SIZE. */
                return log_info_errno(SYNTHETIC_ERRNO(EBADSLT),
                                      "Resource limits disable core dumping for process %s (%s).",
                                      context->meta[META_ARGV_PID], context->meta[META_COMM]);

        process_limit = MAX(arg_process_size_max, storage_size_max());
        if (process_limit == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EBADSLT),
                                       "Limits for coredump processing and storage are both 0, not dumping core.");

        /* Never store more than the process configured, or than we actually shall keep or process */
        max_size = MIN(rlimit, process_limit);

        r = make_filename(context, &fn);
        if (r < 0)
                return log_error_errno(r, "Failed to determine coredump file name: %m");

        (void) mkdir_p_label("/var/lib/systemd/coredump", 0755);

        /* Is compression enabled? Then compress on-the-fly, to keep memory footprint down. */
#if HAVE_COMPRESSION
        if (arg_compress) {
                _cleanup_free_ char *fn_compressed = NULL, *tmp_compressed = NULL;
                _cleanup_close_ int fd_compressed = -1;
                uint64_t uncompressed_size = 0;

                fn_compressed = strjoin(fn, COMPRESSED_EXT);
                if (!fn_compressed) {
                        log_oom();
                        goto uncompressed;
                }

                fd_compressed = open_tmpfile_linkable(fn_compressed, O_RDWR|O_CLOEXEC, &tmp_compressed);
                if (fd_compressed < 0) {
                        log_error_errno(fd_compressed, "Failed to create temporary file for coredump %s: %m", fn_compressed);
                        goto uncompressed;
                }

                /* At this point input_fd might have advanced, so it is too late to fallback to
                 * uncompressed storage. */
                r = compress_stream(input_fd, fd_compressed, max_size, &uncompressed_size);
                if (r < 0) {
                        log_error_errno(r, "Failed to compress %s: %m", coredump_tmpfile_name(tmp_compressed));
                        goto fail_compressed;
                }

                r = fix_permissions(fd_compressed, tmp_compressed, fn_compressed, context, uid);
                if (r < 0)
                        goto fail_compressed;

                if (fstat(fd_compressed, &st) < 0) {
                        r = log_error_errno(errno,
                                            "Failed to fstat core file %s: %m",
                                            coredump_tmpfile_name(tmp_compressed));
                        goto fail_compressed;
                }

                /* Now decompress it again - why? Because the cores are coming from STDIN, so we cannot seek back
                 * to the start. We don't want to keep copies mmapped around, as cores might be huge and cause
                 * large spikes in systemd-coredump's memory footprint. So try to stream-decompress the archive
                 * if possible, and if not we'll just skip saving the backtrace in the journal.
                 * We still observe the maximum storage setting, even if the file lives for a very short
                 * amount of time, since if the storage is on tmpfs it will be charged against coredump's
                 * memory accounting.
                 * This is attempted in a best-effort fashion, in case anything goes wrong we log and
                 * carry on. The uncompressed core is also useful only for journal storage and backtrace
                 * generation, so only do that if either of these is enabled. */
                if ((arg_storage == COREDUMP_STORAGE_JOURNAL && uncompressed_size <= arg_journal_size_max) ||
                    uncompressed_size <= arg_process_size_max) {
                        fd = open_tmpfile_linkable(fn, O_RDWR|O_CLOEXEC, &tmp);
                        if (fd < 0)
                                log_warning_errno(fd,
                                                  "Failed to create temporary file for coredump %s, will not extract backtrace: %m",
                                                  fn);
                        else {
                                r = decompress_stream(fn_compressed, fd_compressed, fd, max_size);
                                if (r < 0) {
                                        log_warning_errno(r,
                                                          "Failed to decompress coredump %s, will not extract backtrace: %m",
                                                          fn);
                                        fd = safe_close(fd);
                                }
                                (void) unlink(tmp);
                        }
                }

                *ret_filename = TAKE_PTR(fn_compressed);       /* compressed */
                *ret_node_fd = TAKE_FD(fd_compressed);         /* compressed */
                *ret_compressed_size = (uint64_t) st.st_size;  /* compressed */
                *ret_data_fd = TAKE_FD(fd);                    /* uncompressed or closed */
                *ret_size = uncompressed_size;                 /* uncompressed */

                return 0;

        fail_compressed:
                if (tmp_compressed)
                        (void) unlink(tmp_compressed);
                return r;
        }

uncompressed:
#endif

        /* If compression is disabled at build time or runtime, then just stream the core
         * file from STDIN to the storage directory. */

        fd = open_tmpfile_linkable(fn, O_RDWR|O_CLOEXEC, &tmp);
        if (fd < 0) {
                r = fd;
                log_error_errno(fd, "Failed to create temporary file for coredump %s: %m", fn);
                goto fail;
        }

        r = copy_bytes(input_fd, fd, max_size, 0);
        if (r < 0) {
                log_error_errno(r, "Cannot store coredump of %s (%s): %m",
                                context->meta[META_ARGV_PID], context->meta[META_COMM]);
                goto fail;
        }
        *ret_truncated = r == 1;
        if (*ret_truncated)
                log_struct(LOG_INFO,
                           LOG_MESSAGE("Core file was truncated to %zu bytes.", max_size),
                           "SIZE_LIMIT=%zu", max_size,
                           "MESSAGE_ID=" SD_MESSAGE_TRUNCATED_CORE_STR);

        if (fstat(fd, &st) < 0) {
                r = -errno;
                log_error_errno(errno, "Failed to fstat core file %s: %m", coredump_tmpfile_name(tmp));
                goto fail;
        }

        r = fix_permissions(fd, tmp, fn, context, uid);
        if (r < 0)
                goto fail;

        *ret_filename = TAKE_PTR(fn);
        *ret_data_fd = TAKE_FD(fd);
        *ret_node_fd = -1;
        *ret_size = (uint64_t) st.st_size;

        return 0;

fail:
        if (tmp)
                (void) unlink(tmp);
        return r;
}

static int allocate_journal_field(int fd, size_t size, char **ret, size_t *ret_size) {
        _cleanup_free_ char *field = NULL;
        ssize_t n;

        assert(fd >= 0);
        assert(ret);
        assert(ret_size);

        if (lseek(fd, 0, SEEK_SET) == (off_t) -1)
                return log_warning_errno(errno, "Failed to seek: %m");

        field = malloc(9 + size);
        if (!field) {
                log_warning("Failed to allocate memory for coredump, coredump will not be stored.");
                return -ENOMEM;
        }

        memcpy(field, "COREDUMP=", 9);

        n = read(fd, field + 9, size);
        if (n < 0)
                return log_error_errno((int) n, "Failed to read core data: %m");
        if ((size_t) n < size)
                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                       "Core data too short.");

        *ret = TAKE_PTR(field);
        *ret_size = size + 9;

        return 0;
}

/* Joins /proc/[pid]/fd/ and /proc/[pid]/fdinfo/ into the following lines:
 * 0:/dev/pts/23
 * pos:    0
 * flags:  0100002
 *
 * 1:/dev/pts/23
 * pos:    0
 * flags:  0100002
 *
 * 2:/dev/pts/23
 * pos:    0
 * flags:  0100002
 * EOF
 */
static int compose_open_fds(pid_t pid, char **open_fds) {
        _cleanup_closedir_ DIR *proc_fd_dir = NULL;
        _cleanup_close_ int proc_fdinfo_fd = -1;
        _cleanup_free_ char *buffer = NULL;
        _cleanup_fclose_ FILE *stream = NULL;
        const char *fddelim = "", *path;
        struct dirent *dent = NULL;
        size_t size = 0;
        int r;

        assert(pid >= 0);
        assert(open_fds != NULL);

        path = procfs_file_alloca(pid, "fd");
        proc_fd_dir = opendir(path);
        if (!proc_fd_dir)
                return -errno;

        proc_fdinfo_fd = openat(dirfd(proc_fd_dir), "../fdinfo", O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC|O_PATH);
        if (proc_fdinfo_fd < 0)
                return -errno;

        stream = open_memstream_unlocked(&buffer, &size);
        if (!stream)
                return -ENOMEM;

        FOREACH_DIRENT(dent, proc_fd_dir, return -errno) {
                _cleanup_fclose_ FILE *fdinfo = NULL;
                _cleanup_free_ char *fdname = NULL;
                _cleanup_close_ int fd = -1;

                r = readlinkat_malloc(dirfd(proc_fd_dir), dent->d_name, &fdname);
                if (r < 0)
                        return r;

                fprintf(stream, "%s%s:%s\n", fddelim, dent->d_name, fdname);
                fddelim = "\n";

                /* Use the directory entry from /proc/[pid]/fd with /proc/[pid]/fdinfo */
                fd = openat(proc_fdinfo_fd, dent->d_name, O_NOFOLLOW|O_CLOEXEC|O_RDONLY);
                if (fd < 0)
                        continue;

                fdinfo = take_fdopen(&fd, "r");
                if (!fdinfo)
                        continue;

                for (;;) {
                        _cleanup_free_ char *line = NULL;

                        r = read_line(fdinfo, LONG_LINE_MAX, &line);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                break;

                        fputs(line, stream);
                        fputc('\n', stream);
                }
        }

        errno = 0;
        stream = safe_fclose(stream);

        if (errno > 0)
                return -errno;

        *open_fds = TAKE_PTR(buffer);

        return 0;
}

static int get_process_ns(pid_t pid, const char *namespace, ino_t *ns) {
        const char *p;
        struct stat stbuf;
        _cleanup_close_ int proc_ns_dir_fd;

        p = procfs_file_alloca(pid, "ns");

        proc_ns_dir_fd = open(p, O_DIRECTORY | O_CLOEXEC | O_RDONLY);
        if (proc_ns_dir_fd < 0)
                return -errno;

        if (fstatat(proc_ns_dir_fd, namespace, &stbuf, /* flags */0) < 0)
                return -errno;

        *ns = stbuf.st_ino;
        return 0;
}

static int get_mount_namespace_leader(pid_t pid, pid_t *container_pid) {
        pid_t cpid = pid, ppid = 0;
        ino_t proc_mntns;
        int r;

        r = get_process_ns(pid, "mnt", &proc_mntns);
        if (r < 0)
                return r;

        for (;;) {
                ino_t parent_mntns;

                r = get_process_ppid(cpid, &ppid);
                if (r < 0)
                        return r;

                r = get_process_ns(ppid, "mnt", &parent_mntns);
                if (r < 0)
                        return r;

                if (proc_mntns != parent_mntns)
                        break;

                if (ppid == 1)
                        return -ENOENT;

                cpid = ppid;
        }

        *container_pid = ppid;
        return 0;
}

/* Returns 1 if the parent was found.
 * Returns 0 if there is not a process we can call the pid's
 * container parent (the pid's process isn't 'containerized').
 * Returns a negative number on errors.
 */
static int get_process_container_parent_cmdline(pid_t pid, char** cmdline) {
        int r = 0;
        pid_t container_pid;
        const char *proc_root_path;
        struct stat root_stat, proc_root_stat;

        /* To compare inodes of / and /proc/[pid]/root */
        if (stat("/", &root_stat) < 0)
                return -errno;

        proc_root_path = procfs_file_alloca(pid, "root");
        if (stat(proc_root_path, &proc_root_stat) < 0)
                return -errno;

        /* The process uses system root. */
        if (proc_root_stat.st_ino == root_stat.st_ino) {
                *cmdline = NULL;
                return 0;
        }

        r = get_mount_namespace_leader(pid, &container_pid);
        if (r < 0)
                return r;

        r = get_process_cmdline(container_pid, SIZE_MAX, 0, cmdline);
        if (r < 0)
                return r;

        return 1;
}

static int change_uid_gid(const Context *context) {
        uid_t uid;
        gid_t gid;
        int r;

        r = parse_uid(context->meta[META_ARGV_UID], &uid);
        if (r < 0)
                return r;

        if (uid_is_system(uid)) {
                const char *user = "systemd-coredump";

                r = get_user_creds(&user, &uid, &gid, NULL, NULL, 0);
                if (r < 0) {
                        log_warning_errno(r, "Cannot resolve %s user. Proceeding to dump core as root: %m", user);
                        uid = gid = 0;
                }
        } else {
                r = parse_gid(context->meta[META_ARGV_GID], &gid);
                if (r < 0)
                        return r;
        }

        return drop_privileges(uid, gid, 0);
}

static int submit_coredump(
                Context *context,
                struct iovec_wrapper *iovw,
                int input_fd) {

        _cleanup_close_ int coredump_fd = -1, coredump_node_fd = -1;
        _cleanup_free_ char *filename = NULL, *coredump_data = NULL;
        _cleanup_free_ char *stacktrace = NULL;
        char *core_message;
        uint64_t coredump_size = UINT64_MAX;
        uint64_t coredump_compressed_size = UINT64_MAX;
        bool truncated = false;
        int r;

        assert(context);
        assert(iovw);
        assert(input_fd >= 0);

        /* Vacuum before we write anything again */
        (void) coredump_vacuum(-1, arg_keep_free, arg_max_use);

        /* Always stream the coredump to disk, if that's possible */
        r = save_external_coredump(context, input_fd,
                                   &filename, &coredump_node_fd, &coredump_fd,
                                   &coredump_size, &coredump_compressed_size, &truncated);
        if (r < 0)
                /* Skip whole core dumping part */
                goto log;

        /* If we don't want to keep the coredump on disk, remove it now, as later on we
         * will lack the privileges for it. However, we keep the fd to it, so that we can
         * still process it and log it. */
        r = maybe_remove_external_coredump(filename, coredump_node_fd >= 0 ? coredump_compressed_size : coredump_size);
        if (r < 0)
                return r;
        if (r == 0) {
                (void) iovw_put_string_field(iovw, "COREDUMP_FILENAME=", filename);

        } else if (arg_storage == COREDUMP_STORAGE_EXTERNAL)
                log_info("The core will not be stored: size %"PRIu64" is greater than %"PRIu64" (the configured maximum)",
                         coredump_node_fd >= 0 ? coredump_compressed_size : coredump_size, arg_external_size_max);

        /* Vacuum again, but exclude the coredump we just created */
        (void) coredump_vacuum(coredump_node_fd >= 0 ? coredump_node_fd : coredump_fd, arg_keep_free, arg_max_use);

        /* Now, let's drop privileges to become the user who owns the segfaulted process
         * and allocate the coredump memory under the user's uid. This also ensures that
         * the credentials journald will see are the ones of the coredumping user, thus
         * making sure the user gets access to the core dump. Let's also get rid of all
         * capabilities, if we run as root, we won't need them anymore. */
        r = change_uid_gid(context);
        if (r < 0)
                return log_error_errno(r, "Failed to drop privileges: %m");

#if HAVE_ELFUTILS
        /* Try to get a stack trace if we can */
        if (coredump_size > arg_process_size_max) {
                log_debug("Not generating stack trace: core size %"PRIu64" is greater "
                          "than %"PRIu64" (the configured maximum)",
                          coredump_size, arg_process_size_max);
        } else if (coredump_fd != -1)
                coredump_make_stack_trace(coredump_fd, context->meta[META_EXE], &stacktrace);
#endif

log:
        core_message = strjoina("Process ", context->meta[META_ARGV_PID],
                                " (", context->meta[META_COMM], ") of user ",
                                context->meta[META_ARGV_UID], " dumped core.",
                                context->is_journald && filename ? "\nCoredump diverted to " : NULL,
                                context->is_journald && filename ? filename : NULL);

        core_message = strjoina(core_message, stacktrace ? "\n\n" : NULL, stacktrace);

        if (context->is_journald) {
                /* We cannot log to the journal, so just print the message.
                 * The target was set previously to something safe. */
                log_dispatch(LOG_ERR, 0, core_message);
                return 0;
        }

        (void) iovw_put_string_field(iovw, "MESSAGE=", core_message);

        if (truncated)
                (void) iovw_put_string_field(iovw, "COREDUMP_TRUNCATED=", "1");

        /* Optionally store the entire coredump in the journal */
        if (arg_storage == COREDUMP_STORAGE_JOURNAL && coredump_fd != -1) {
                if (coredump_size <= arg_journal_size_max) {
                        size_t sz = 0;

                        /* Store the coredump itself in the journal */

                        r = allocate_journal_field(coredump_fd, (size_t) coredump_size, &coredump_data, &sz);
                        if (r >= 0) {
                                if (iovw_put(iovw, coredump_data, sz) >= 0)
                                        TAKE_PTR(coredump_data);
                        } else
                                log_warning_errno(r, "Failed to attach the core to the journal entry: %m");
                } else
                        log_info("The core will not be stored: size %"PRIu64" is greater than %"PRIu64" (the configured maximum)",
                                 coredump_size, arg_journal_size_max);
        }

        r = sd_journal_sendv(iovw->iovec, iovw->count);
        if (r < 0)
                return log_error_errno(r, "Failed to log coredump: %m");

        return 0;
}

static int save_context(Context *context, const struct iovec_wrapper *iovw) {
        unsigned count = 0;
        const char *unit;
        int r;

        assert(context);
        assert(iovw);
        assert(iovw->count >= _META_ARGV_MAX);

        /* The context does not allocate any memory on its own */

        for (size_t n = 0; n < iovw->count; n++) {
                struct iovec *iovec = iovw->iovec + n;

                for (size_t i = 0; i < ELEMENTSOF(meta_field_names); i++) {
                        char *p;

                        /* Note that these strings are NUL terminated, because we made sure that a
                         * trailing NUL byte is in the buffer, though not included in the iov_len
                         * count (see process_socket() and gather_pid_metadata_*()) */
                        assert(((char*) iovec->iov_base)[iovec->iov_len] == 0);

                        p = startswith(iovec->iov_base, meta_field_names[i]);
                        if (p) {
                                context->meta[i] = p;
                                count++;
                                break;
                        }
                }
        }

        if (!context->meta[META_ARGV_PID])
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Failed to find the PID of crashing process");

        r = parse_pid(context->meta[META_ARGV_PID], &context->pid);
        if (r < 0)
                return log_error_errno(r, "Failed to parse PID \"%s\": %m", context->meta[META_ARGV_PID]);

        unit = context->meta[META_UNIT];
        context->is_pid1 = streq(context->meta[META_ARGV_PID], "1") || streq_ptr(unit, SPECIAL_INIT_SCOPE);
        context->is_journald = streq_ptr(unit, SPECIAL_JOURNALD_SERVICE);

        return 0;
}

static int process_socket(int fd) {
        _cleanup_close_ int input_fd = -1;
        Context context = {};
        struct iovec_wrapper iovw = {};
        struct iovec iovec;
        int r;

        assert(fd >= 0);

        log_setup();

        log_debug("Processing coredump received on stdin...");

        for (;;) {
                CMSG_BUFFER_TYPE(CMSG_SPACE(sizeof(int))) control;
                struct msghdr mh = {
                        .msg_control = &control,
                        .msg_controllen = sizeof(control),
                        .msg_iovlen = 1,
                };
                ssize_t n;
                ssize_t l;

                l = next_datagram_size_fd(fd);
                if (l < 0) {
                        r = log_error_errno(l, "Failed to determine datagram size to read: %m");
                        goto finish;
                }

                iovec.iov_len = l;
                iovec.iov_base = malloc(l + 1);
                if (!iovec.iov_base) {
                        r = log_oom();
                        goto finish;
                }

                mh.msg_iov = &iovec;

                n = recvmsg_safe(fd, &mh, MSG_CMSG_CLOEXEC);
                if (n < 0)  {
                        free(iovec.iov_base);
                        r = log_error_errno(n, "Failed to receive datagram: %m");
                        goto finish;
                }

                /* The final zero-length datagram carries the file descriptor and tells us
                 * that we're done. */
                if (n == 0) {
                        struct cmsghdr *found;

                        free(iovec.iov_base);

                        found = cmsg_find(&mh, SOL_SOCKET, SCM_RIGHTS, CMSG_LEN(sizeof(int)));
                        if (!found) {
                                cmsg_close_all(&mh);
                                r = log_error_errno(SYNTHETIC_ERRNO(EBADMSG),
                                                    "Coredump file descriptor missing.");
                                goto finish;
                        }

                        assert(input_fd < 0);
                        input_fd = *(int*) CMSG_DATA(found);
                        break;
                } else
                        cmsg_close_all(&mh);

                /* Add trailing NUL byte, in case these are strings */
                ((char*) iovec.iov_base)[n] = 0;
                iovec.iov_len = (size_t) n;

                r = iovw_put(&iovw, iovec.iov_base, iovec.iov_len);
                if (r < 0)
                        goto finish;
        }

        /* Make sure we got all data we really need */
        assert(input_fd >= 0);

        r = save_context(&context, &iovw);
        if (r < 0)
                goto finish;

        /* Make sure we received at least all fields we need. */
        for (int i = 0; i < _META_MANDATORY_MAX; i++)
                if (!context.meta[i]) {
                        r = log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                            "A mandatory argument (%i) has not been sent, aborting.",
                                            i);
                        goto finish;
                }

        r = submit_coredump(&context, &iovw, input_fd);

finish:
        iovw_free_contents(&iovw, true);
        return r;
}

static int send_iovec(const struct iovec_wrapper *iovw, int input_fd) {

        static const union sockaddr_union sa = {
                .un.sun_family = AF_UNIX,
                .un.sun_path = "/run/systemd/coredump",
        };
        _cleanup_close_ int fd = -1;
        int r;

        assert(iovw);
        assert(input_fd >= 0);

        fd = socket(AF_UNIX, SOCK_SEQPACKET|SOCK_CLOEXEC, 0);
        if (fd < 0)
                return log_error_errno(errno, "Failed to create coredump socket: %m");

        if (connect(fd, &sa.sa, SOCKADDR_UN_LEN(sa.un)) < 0)
                return log_error_errno(errno, "Failed to connect to coredump service: %m");

        for (size_t i = 0; i < iovw->count; i++) {
                struct msghdr mh = {
                        .msg_iov = iovw->iovec + i,
                        .msg_iovlen = 1,
                };
                struct iovec copy[2];

                for (;;) {
                        if (sendmsg(fd, &mh, MSG_NOSIGNAL) >= 0)
                                break;

                        if (errno == EMSGSIZE && mh.msg_iov[0].iov_len > 0) {
                                /* This field didn't fit? That's a pity. Given that this is
                                 * just metadata, let's truncate the field at half, and try
                                 * again. We append three dots, in order to show that this is
                                 * truncated. */

                                if (mh.msg_iov != copy) {
                                        /* We don't want to modify the caller's iovec, hence
                                         * let's create our own array, consisting of two new
                                         * iovecs, where the first is a (truncated) copy of
                                         * what we want to send, and the second one contains
                                         * the trailing dots. */
                                        copy[0] = iovw->iovec[i];
                                        copy[1] = IOVEC_MAKE(((char[]){'.', '.', '.'}), 3);

                                        mh.msg_iov = copy;
                                        mh.msg_iovlen = 2;
                                }

                                copy[0].iov_len /= 2; /* halve it, and try again */
                                continue;
                        }

                        return log_error_errno(errno, "Failed to send coredump datagram: %m");
                }
        }

        r = send_one_fd(fd, input_fd, 0);
        if (r < 0)
                return log_error_errno(r, "Failed to send coredump fd: %m");

        return 0;
}

static int gather_pid_metadata_from_argv(
                struct iovec_wrapper *iovw,
                Context *context,
                int argc, char **argv) {

        _cleanup_free_ char *free_timestamp = NULL;
        int r, signo;
        char *t;

        /* We gather all metadata that were passed via argv[] into an array of iovecs that
         * we'll forward to the socket unit */

        if (argc < _META_ARGV_MAX)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Not enough arguments passed by the kernel (%i, expected %i).",
                                       argc, _META_ARGV_MAX);

        for (int i = 0; i < _META_ARGV_MAX; i++) {

                t = argv[i];

                switch (i) {

                case META_ARGV_TIMESTAMP:
                        /* The journal fields contain the timestamp padded with six
                         * zeroes, so that the kernel-supplied 1s granularity timestamps
                         * becomes 1µs granularity, i.e. the granularity systemd usually
                         * operates in. */
                        t = free_timestamp = strjoin(argv[i], "000000");
                        if (!t)
                                return log_oom();
                        break;

                case META_ARGV_SIGNAL:
                        /* For signal, record its pretty name too */
                        if (safe_atoi(argv[i], &signo) >= 0 && SIGNAL_VALID(signo))
                                (void) iovw_put_string_field(iovw, "COREDUMP_SIGNAL_NAME=SIG",
                                                             signal_to_string(signo));
                        break;

                default:
                        break;
                }

                r = iovw_put_string_field(iovw, meta_field_names[i], t);
                if (r < 0)
                        return r;
        }

        /* Cache some of the process metadata we collected so far and that we'll need to
         * access soon */
        return save_context(context, iovw);
}

static int gather_pid_metadata(struct iovec_wrapper *iovw, Context *context) {
        uid_t owner_uid;
        pid_t pid;
        char *t;
        const char *p;
        int r;

        /* Note that if we fail on oom later on, we do not roll-back changes to the iovec
         * structure. (It remains valid, with the first iovec fields initialized.) */

        pid = context->pid;

        /* The following is mandatory */
        r = get_process_comm(pid, &t);
        if (r < 0)
                return log_error_errno(r, "Failed to get COMM: %m");

        r = iovw_put_string_field_free(iovw, "COREDUMP_COMM=", t);
        if (r < 0)
                return r;

        /* The following are optional but we used them if present */
        r = get_process_exe(pid, &t);
        if (r >= 0)
                r = iovw_put_string_field_free(iovw, "COREDUMP_EXE=", t);
        if (r < 0)
                log_warning_errno(r, "Failed to get EXE, ignoring: %m");

        if (cg_pid_get_unit(pid, &t) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_UNIT=", t);

        /* The next are optional */
        if (cg_pid_get_user_unit(pid, &t) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_USER_UNIT=", t);

        if (sd_pid_get_session(pid, &t) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_SESSION=", t);

        if (sd_pid_get_owner_uid(pid, &owner_uid) >= 0) {
                r = asprintf(&t, UID_FMT, owner_uid);
                if (r > 0)
                        (void) iovw_put_string_field_free(iovw, "COREDUMP_OWNER_UID=", t);
        }

        if (sd_pid_get_slice(pid, &t) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_SLICE=", t);

        if (get_process_cmdline(pid, SIZE_MAX, 0, &t) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_CMDLINE=", t);

        if (cg_pid_get_path_shifted(pid, NULL, &t) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_CGROUP=", t);

        if (compose_open_fds(pid, &t) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_OPEN_FDS=", t);

        p = procfs_file_alloca(pid, "status");
        if (read_full_virtual_file(p, &t, NULL) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_PROC_STATUS=", t);

        p = procfs_file_alloca(pid, "maps");
        if (read_full_virtual_file(p, &t, NULL) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_PROC_MAPS=", t);

        p = procfs_file_alloca(pid, "limits");
        if (read_full_virtual_file(p, &t, NULL) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_PROC_LIMITS=", t);

        p = procfs_file_alloca(pid, "cgroup");
        if (read_full_virtual_file(p, &t, NULL) >=0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_PROC_CGROUP=", t);

        p = procfs_file_alloca(pid, "mountinfo");
        if (read_full_virtual_file(p, &t, NULL) >=0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_PROC_MOUNTINFO=", t);

        if (get_process_cwd(pid, &t) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_CWD=", t);

        if (get_process_root(pid, &t) >= 0) {
                bool proc_self_root_is_slash;

                proc_self_root_is_slash = strcmp(t, "/") == 0;

                (void) iovw_put_string_field_free(iovw, "COREDUMP_ROOT=", t);

                /* If the process' root is "/", then there is a chance it has
                 * mounted own root and hence being containerized. */
                if (proc_self_root_is_slash && get_process_container_parent_cmdline(pid, &t) > 0)
                        (void) iovw_put_string_field_free(iovw, "COREDUMP_CONTAINER_CMDLINE=", t);
        }

        if (get_process_environ(pid, &t) >= 0)
                (void) iovw_put_string_field_free(iovw, "COREDUMP_ENVIRON=", t);

        /* we successfully acquired all metadata */
        return save_context(context, iovw);
}

static int process_kernel(int argc, char* argv[]) {
        Context context = {};
        struct iovec_wrapper *iovw;
        int r;

        log_debug("Processing coredump received from the kernel...");

        iovw = iovw_new();
        if (!iovw)
                return log_oom();

        (void) iovw_put_string_field(iovw, "MESSAGE_ID=", SD_MESSAGE_COREDUMP_STR);
        (void) iovw_put_string_field(iovw, "PRIORITY=", STRINGIFY(LOG_CRIT));

        /* Collect all process metadata passed by the kernel through argv[] */
        r = gather_pid_metadata_from_argv(iovw, &context, argc - 1, argv + 1);
        if (r < 0)
                goto finish;

        /* Collect the rest of the process metadata retrieved from the runtime */
        r = gather_pid_metadata(iovw, &context);
        if (r < 0)
                goto finish;

        if (!context.is_journald) {
                /* OK, now we know it's not the journal, hence we can make use of it now. */
                log_set_target(LOG_TARGET_JOURNAL_OR_KMSG);
                log_open();
        }

        /* If this is PID 1 disable coredump collection, we'll unlikely be able to process
         * it later on.
         *
         * FIXME: maybe we should disable coredumps generation from the beginning and
         * re-enable it only when we know it's either safe (ie we're not running OOM) or
         * it's not pid1 ? */
        if (context.is_pid1) {
                log_notice("Due to PID 1 having crashed coredump collection will now be turned off.");
                disable_coredumps();
        }

        if (context.is_journald || context.is_pid1)
                r = submit_coredump(&context, iovw, STDIN_FILENO);
        else
                r = send_iovec(iovw, STDIN_FILENO);

 finish:
        iovw = iovw_free_free(iovw);
        return r;
}

static int process_backtrace(int argc, char *argv[]) {
        Context context = {};
        struct iovec_wrapper *iovw;
        char *message;
        int r;
         _cleanup_(journal_importer_cleanup) JournalImporter importer = JOURNAL_IMPORTER_INIT(STDIN_FILENO);

        log_debug("Processing backtrace on stdin...");

        iovw = iovw_new();
        if (!iovw)
                return log_oom();

        (void) iovw_put_string_field(iovw, "MESSAGE_ID=", SD_MESSAGE_BACKTRACE_STR);
        (void) iovw_put_string_field(iovw, "PRIORITY=", STRINGIFY(LOG_CRIT));

        /* Collect all process metadata from argv[] by making sure to skip the
         * '--backtrace' option */
        r = gather_pid_metadata_from_argv(iovw, &context, argc - 2, argv + 2);
        if (r < 0)
                goto finish;

        /* Collect the rest of the process metadata retrieved from the runtime */
        r = gather_pid_metadata(iovw, &context);
        if (r < 0)
                goto finish;

        for (;;) {
                r = journal_importer_process_data(&importer);
                if (r < 0) {
                        log_error_errno(r, "Failed to parse journal entry on stdin: %m");
                        goto finish;
                }
                if (r == 1 ||                        /* complete entry */
                    journal_importer_eof(&importer)) /* end of data */
                        break;
        }

        if (journal_importer_eof(&importer)) {
                log_warning("Did not receive a full journal entry on stdin, ignoring message sent by reporter");

                message = strjoina("Process ", context.meta[META_ARGV_PID],
                                  " (", context.meta[META_COMM], ")"
                                  " of user ", context.meta[META_ARGV_UID],
                                  " failed with ", context.meta[META_ARGV_SIGNAL]);

                r = iovw_put_string_field(iovw, "MESSAGE=", message);
                if (r < 0)
                        return r;
        } else {
                /* The imported iovecs are not supposed to be freed by us so let's store
                 * them at the end of the array so we can skip them while freeing the
                 * rest. */
                for (size_t i = 0; i < importer.iovw.count; i++) {
                        struct iovec *iovec = importer.iovw.iovec + i;

                        iovw_put(iovw, iovec->iov_base, iovec->iov_len);
                }
        }

        r = sd_journal_sendv(iovw->iovec, iovw->count);
        if (r < 0)
                log_error_errno(r, "Failed to log backtrace: %m");

 finish:
        iovw->count -= importer.iovw.count;
        iovw = iovw_free_free(iovw);
        return r;
}

static int run(int argc, char *argv[]) {
        int r;

        /* First, log to a safe place, since we don't know what crashed and it might
         * be journald which we'd rather not log to then. */

        log_set_target(LOG_TARGET_KMSG);
        log_open();

        /* Make sure we never enter a loop */
        (void) prctl(PR_SET_DUMPABLE, 0);

        /* Ignore all parse errors */
        (void) parse_config();

        log_debug("Selected storage '%s'.", coredump_storage_to_string(arg_storage));
        log_debug("Selected compression %s.", yes_no(arg_compress));

        r = sd_listen_fds(false);
        if (r < 0)
                return log_error_errno(r, "Failed to determine the number of file descriptors: %m");

        /* If we got an fd passed, we are running in coredumpd mode. Otherwise we
         * are invoked from the kernel as coredump handler. */
        if (r == 0) {
                if (streq_ptr(argv[1], "--backtrace"))
                        return process_backtrace(argc, argv);
                else
                        return process_kernel(argc, argv);
        } else if (r == 1)
                return process_socket(SD_LISTEN_FDS_START);

        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                               "Received unexpected number of file descriptors.");
}

DEFINE_MAIN_FUNCTION(run);
