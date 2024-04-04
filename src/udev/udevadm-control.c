/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <errno.h>
#include <getopt.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "copy.h"
#include "creds-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "fs-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "recurse-dir.h"
#include "static-destruct.h"
#include "strv.h"
#include "syslog-util.h"
#include "time-util.h"
#include "udevadm.h"
#include "udev-ctrl.h"
#include "virt.h"

#define UDEV_RULES_RUNTIME_DIR "/run/udev/rules.d/"

static char **arg_env = NULL;
static usec_t arg_timeout = 60 * USEC_PER_SEC;
static bool arg_ping = false;
static bool arg_reload = false;
static bool arg_exit = false;
static int arg_max_children = -1;
static int arg_log_level = -1;
static int arg_start_exec_queue = -1;
static int arg_has_control_commands = false;
static int arg_load_credentials = false;

STATIC_DESTRUCTOR_REGISTER(arg_env, strv_freep);

static int help(void) {
        printf("%s control OPTION\n\n"
               "Control the udev daemon.\n\n"
               "  -h --help                Show this help\n"
               "  -V --version             Show package version\n"
               "  -e --exit                Instruct the daemon to cleanup and exit\n"
               "  -l --log-level=LEVEL     Set the udev log level for the daemon\n"
               "  -s --stop-exec-queue     Do not execute events, queue only\n"
               "  -S --start-exec-queue    Execute events, flush queue\n"
               "  -R --reload              Reload rules and databases\n"
               "  -p --property=KEY=VALUE  Set a global property for all events\n"
               "  -m --children-max=N      Maximum number of children\n"
               "     --ping                Wait for udev to respond to a ping message\n"
               "  -t --timeout=SECONDS     Maximum time to block for a reply\n"
               "  -L --load-credentials    Load udev rules from credentials\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_PING = 0x100,
        };

        static const struct option options[] = {
                { "exit",             no_argument,       NULL, 'e'      },
                { "log-level",        required_argument, NULL, 'l'      },
                { "log-priority",     required_argument, NULL, 'l'      }, /* for backward compatibility */
                { "stop-exec-queue",  no_argument,       NULL, 's'      },
                { "start-exec-queue", no_argument,       NULL, 'S'      },
                { "reload",           no_argument,       NULL, 'R'      },
                { "reload-rules",     no_argument,       NULL, 'R'      }, /* alias for -R */
                { "property",         required_argument, NULL, 'p'      },
                { "env",              required_argument, NULL, 'p'      }, /* alias for -p */
                { "children-max",     required_argument, NULL, 'm'      },
                { "ping",             no_argument,       NULL, ARG_PING },
                { "timeout",          required_argument, NULL, 't'      },
                { "load-credentials", no_argument,       NULL, 'L'      },
                { "version",          no_argument,       NULL, 'V'      },
                { "help",             no_argument,       NULL, 'h'      },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "el:sSRp:m:t:LVh", options, NULL)) >= 0)
                switch (c) {

                case 'e':
                        arg_exit = true;
                        arg_has_control_commands = true;
                        break;

                case 'l':
                        arg_log_level = log_level_from_string(optarg);
                        if (arg_log_level < 0)
                                return log_error_errno(arg_log_level, "Failed to parse log level '%s': %m", optarg);
                        arg_has_control_commands = true;
                        break;

                case 's':
                        arg_start_exec_queue = false;
                        arg_has_control_commands = true;
                        break;

                case 'S':
                        arg_start_exec_queue = true;
                        arg_has_control_commands = true;
                        break;

                case 'R':
                        arg_reload = true;
                        arg_has_control_commands = true;
                        break;

                case 'p':
                        if (!strchr(optarg, '='))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "expect <KEY>=<value> instead of '%s'", optarg);

                        r = strv_extend(&arg_env, optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to extend environment: %m");

                        arg_has_control_commands = true;
                        break;

                case 'm': {
                        unsigned i;
                        r = safe_atou(optarg, &i);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse maximum number of children '%s': %m", optarg);
                        arg_max_children = i;
                        arg_has_control_commands = true;
                        break;
                }

                case ARG_PING:
                        arg_ping = true;
                        arg_has_control_commands = true;
                        break;

                case 't':
                        r = parse_sec(optarg, &arg_timeout);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse timeout value '%s': %m", optarg);
                        break;

                case 'L':
                        arg_load_credentials = true;
                        break;

                case 'V':
                        return print_version();

                case 'h':
                        return help();

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (!arg_has_control_commands && !arg_load_credentials)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "No control command option is specified.");

        if (optind < argc)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Extraneous argument: %s", argv[optind]);

        return 1;
}

static int pick_up_credentials(void) {
        _cleanup_close_ int credential_dir_fd = -EBADF, udev_rules_dir_fd = -EBADF;
        int r, ret = 0;

        credential_dir_fd = open_credentials_dir();
        if (IN_SET(credential_dir_fd, -ENXIO, -ENOENT)) {
                /* Credential env var not set, or dir doesn't exist. */
                log_debug("No credentials found.");
                return 0;
        }
        if (credential_dir_fd < 0)
                return log_error_errno(credential_dir_fd, "Failed to open credentials directory: %m");

        _cleanup_free_ DirectoryEntries *des = NULL;
        r = readdir_all(credential_dir_fd, RECURSE_DIR_SORT|RECURSE_DIR_IGNORE_DOT|RECURSE_DIR_ENSURE_TYPE, &des);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate credentials: %m");

        FOREACH_ARRAY(i, des->entries, des->n_entries) {
                struct dirent *de = *i;

                if (de->d_type != DT_REG)
                        continue;

                const char *e = startswith(de->d_name, "udev.rules.");
                if (!e)
                        continue;

                _cleanup_free_ char *fn = strjoin(e, ".rules");
                if (!fn)
                        return log_oom();

                if (!filename_is_valid(fn)) {
                        log_warning("Passed credential '%s' would result in invalid filename '%s', ignoring.", de->d_name, fn);
                        continue;
                }

                if (udev_rules_dir_fd < 0) {
                        udev_rules_dir_fd = xopenat_full(AT_FDCWD, UDEV_RULES_RUNTIME_DIR, O_CLOEXEC | O_CREAT | O_DIRECTORY, 0, 0755);
                        if (udev_rules_dir_fd < 0)
                                return log_error_errno(udev_rules_dir_fd, "Failed to open "UDEV_RULES_RUNTIME_DIR": %m");
                }

                r = copy_file_at(
                                credential_dir_fd, de->d_name,
                                udev_rules_dir_fd, fn,
                                /* open_flags= */ 0,
                                0644,
                                /* flags= */ 0);
                if (r < 0)
                        RET_GATHER(ret, log_warning_errno(r, "Failed to copy credential %s → file "UDEV_RULES_RUNTIME_DIR"%s: %m", de->d_name, fn));
                else
                        log_info("Installed "UDEV_RULES_RUNTIME_DIR"%s from credential.", fn);
        }

        return ret;
}

static int send_control_commands(void) {
        _cleanup_(udev_ctrl_unrefp) UdevCtrl *uctrl = NULL;
        int r;

        r = udev_ctrl_new(&uctrl);
        if (r < 0)
                return log_error_errno(r, "Failed to initialize udev control: %m");

        if (arg_exit) {
                r = udev_ctrl_send_exit(uctrl);
                if (r < 0)
                       return log_error_errno(r, "Failed to send exit request: %m");
                return 0;
        }

        if (arg_log_level >= 0) {
                r = udev_ctrl_send_set_log_level(uctrl, arg_log_level);
                if (r < 0)
                        return log_error_errno(r, "Failed to send request to set log level: %m");
        }

        if (arg_start_exec_queue == false) {
                r = udev_ctrl_send_stop_exec_queue(uctrl);
                if (r < 0)
                        return log_error_errno(r, "Failed to send request to stop exec queue: %m");
        }

        if (arg_start_exec_queue == true) {
                r = udev_ctrl_send_start_exec_queue(uctrl);
                if (r < 0)
                        return log_error_errno(r, "Failed to send request to start exec queue: %m");
        }

        if (arg_reload) {
                r = udev_ctrl_send_reload(uctrl);
                if (r < 0)
                        return log_error_errno(r, "Failed to send reload request: %m");
        }

        STRV_FOREACH(env, arg_env) {
                r = udev_ctrl_send_set_env(uctrl, *env);
                if (r < 0)
                        return log_error_errno(r, "Failed to send request to update environment: %m");
        }

        if (arg_max_children >= 0) {
                r = udev_ctrl_send_set_children_max(uctrl, arg_max_children);
                if (r < 0)
                        return log_error_errno(r, "Failed to send request to set number of children: %m");
        }

        if (arg_ping) {
                r = udev_ctrl_send_ping(uctrl);
                if (r < 0)
                        return log_error_errno(r, "Failed to send a ping message: %m");
        }

        r = udev_ctrl_wait(uctrl, arg_timeout);
        if (r < 0)
                return log_error_errno(r, "Failed to wait for daemon to reply: %m");

        return 0;
}

int control_main(int argc, char *argv[], void *userdata) {
        int r;

        if (running_in_chroot() > 0) {
                log_info("Running in chroot, ignoring request.");
                return 0;
        }

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        if (arg_load_credentials) {
                r = pick_up_credentials();
                if (r < 0)
                        return r;
        }

        if (arg_has_control_commands) {
                r = send_control_commands();
                if (r < 0)
                        return r;
        }

        return 0;
}
