/***
  This file is part of systemd.

  Copyright 2012 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <mntent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "alloc-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "fstab-util.h"
#include "generator.h"
#include "log.h"
#include "mkdir.h"
#include "mount-setup.h"
#include "mount-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "unit-name.h"
#include "util.h"
#include "virt.h"

static const char *arg_dest = "/tmp";
static bool arg_fstab_enabled = true;
static char *arg_root_what = NULL;
static char *arg_root_fstype = NULL;
static char *arg_root_options = NULL;
static int arg_root_rw = -1;
static char *arg_usr_what = NULL;
static char *arg_usr_fstype = NULL;
static char *arg_usr_options = NULL;

static int add_swap(
                const char *what,
                struct mntent *me,
                bool noauto,
                bool nofail) {

        _cleanup_free_ char *name = NULL, *unit = NULL, *lnk = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(what);
        assert(me);

        if (access("/proc/swaps", F_OK) < 0) {
                log_info("Swap not supported, ignoring fstab swap entry for %s.", what);
                return 0;
        }

        if (detect_container() > 0) {
                log_info("Running in a container, ignoring fstab swap entry for %s.", what);
                return 0;
        }

        r = unit_name_from_path(what, ".swap", &name);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                if (errno == EEXIST)
                        log_error("Failed to create swap unit file %s, as it already exists. Duplicate entry in /etc/fstab?", unit);
                else
                        log_error_errno(errno, "Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fprintf(f,
                "# Automatically generated by systemd-fstab-generator\n\n"
                "[Unit]\n"
                "SourcePath=/etc/fstab\n"
                "Documentation=man:fstab(5) man:systemd-fstab-generator(8)\n\n"
                "[Swap]\n"
                "What=%s\n",
                what);

        if (!isempty(me->mnt_opts) && !streq(me->mnt_opts, "defaults"))
                fprintf(f, "Options=%s\n", me->mnt_opts);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        /* use what as where, to have a nicer error message */
        r = generator_write_timeouts(arg_dest, what, what, me->mnt_opts, NULL);
        if (r < 0)
                return r;

        if (!noauto) {
                lnk = strjoin(arg_dest, "/" SPECIAL_SWAP_TARGET,
                              nofail ? ".wants/" : ".requires/", name, NULL);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(unit, lnk) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);
        }

        return 0;
}

static bool mount_is_network(struct mntent *me) {
        assert(me);

        return fstab_test_option(me->mnt_opts, "_netdev\0") ||
               fstype_is_network(me->mnt_type);
}

static bool mount_in_initrd(struct mntent *me) {
        assert(me);

        return fstab_test_option(me->mnt_opts, "x-initrd.mount\0") ||
               streq(me->mnt_dir, "/usr");
}

static int write_idle_timeout(FILE *f, const char *where, const char *opts) {
        _cleanup_free_ char *timeout = NULL;
        char timespan[FORMAT_TIMESPAN_MAX];
        usec_t u;
        int r;

        r = fstab_filter_options(opts, "x-systemd.idle-timeout\0", NULL, &timeout, NULL);
        if (r < 0)
                return log_warning_errno(r, "Failed to parse options: %m");
        if (r == 0)
                return 0;

        r = parse_sec(timeout, &u);
        if (r < 0) {
                log_warning("Failed to parse timeout for %s, ignoring: %s", where, timeout);
                return 0;
        }

        fprintf(f, "TimeoutIdleSec=%s\n", format_timespan(timespan, sizeof(timespan), u, 0));

        return 0;
}

static int write_requires_after(FILE *f, const char *opts) {
        _cleanup_strv_free_ char **names = NULL, **units = NULL;
        _cleanup_free_ char *res = NULL;
        char **s;
        int r;

        assert(f);
        assert(opts);

        r = fstab_extract_values(opts, "x-systemd.requires", &names);
        if (r < 0)
                return log_warning_errno(r, "Failed to parse options: %m");
        if (r == 0)
                return 0;

        STRV_FOREACH(s, names) {
                char *x;

                r = unit_name_mangle_with_suffix(*s, UNIT_NAME_NOGLOB, ".mount", &x);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate unit name: %m");
                r = strv_consume(&units, x);
                if (r < 0)
                        return log_oom();
        }

        if (units) {
                res = strv_join(units, " ");
                if (!res)
                        return log_oom();
                fprintf(f, "After=%1$s\nRequires=%1$s\n", res);
        }

        return 0;
}

static int write_requires_mounts_for(FILE *f, const char *opts) {
        _cleanup_strv_free_ char **paths = NULL;
        _cleanup_free_ char *res = NULL;
        int r;

        assert(f);
        assert(opts);

        r = fstab_extract_values(opts, "x-systemd.requires-mounts-for", &paths);
        if (r < 0)
                return log_warning_errno(r, "Failed to parse options: %m");
        if (r == 0)
                return 0;

        res = strv_join(paths, " ");
        if (!res)
                return log_oom();

        fprintf(f, "RequiresMountsFor=%s\n", res);

        return 0;
}

static int add_mount(
                const char *what,
                const char *where,
                const char *fstype,
                const char *opts,
                int passno,
                bool noauto,
                bool nofail,
                bool automount,
                const char *post,
                const char *source) {

        _cleanup_free_ char
                *name = NULL, *unit = NULL, *lnk = NULL,
                *automount_name = NULL, *automount_unit = NULL,
                *filtered = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(what);
        assert(where);
        assert(opts);
        assert(post);
        assert(source);

        if (streq_ptr(fstype, "autofs"))
                return 0;

        if (!is_path(where)) {
                log_warning("Mount point %s is not a valid path, ignoring.", where);
                return 0;
        }

        if (mount_point_is_api(where) ||
            mount_point_ignore(where))
                return 0;

        if (path_equal(where, "/")) {
                if (noauto)
                        log_warning("Ignoring \"noauto\" for root device");
                if (nofail)
                        log_warning("Ignoring \"nofail\" for root device");
                if (automount)
                        log_warning("Ignoring automount option for root device");

                noauto = nofail = automount = false;
        }

        r = unit_name_from_path(where, ".mount", &name);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        unit = strjoin(arg_dest, "/", name, NULL);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f) {
                if (errno == EEXIST)
                        log_error("Failed to create mount unit file %s, as it already exists. Duplicate entry in /etc/fstab?", unit);
                else
                        log_error_errno(errno, "Failed to create unit file %s: %m", unit);
                return -errno;
        }

        fprintf(f,
                "# Automatically generated by systemd-fstab-generator\n\n"
                "[Unit]\n"
                "SourcePath=%s\n"
                "Documentation=man:fstab(5) man:systemd-fstab-generator(8)\n",
                source);

        if (!noauto && !nofail && !automount)
                fprintf(f, "Before=%s\n", post);

        if (!automount && opts) {
                 r = write_requires_after(f, opts);
                 if (r < 0)
                         return r;
                 r = write_requires_mounts_for(f, opts);
                 if (r < 0)
                         return r;
        }

        if (passno != 0) {
                r = generator_write_fsck_deps(f, arg_dest, what, where, fstype);
                if (r < 0)
                        return r;
        }

        fprintf(f,
                "\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=%s\n",
                what,
                where);

        if (!isempty(fstype) && !streq(fstype, "auto"))
                fprintf(f, "Type=%s\n", fstype);

        r = generator_write_timeouts(arg_dest, what, where, opts, &filtered);
        if (r < 0)
                return r;

        if (!isempty(filtered) && !streq(filtered, "defaults"))
                fprintf(f, "Options=%s\n", filtered);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        if (!noauto && !automount) {
                lnk = strjoin(arg_dest, "/", post, nofail ? ".wants/" : ".requires/", name, NULL);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(unit, lnk) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);
        }

        if (automount) {
                r = unit_name_from_path(where, ".automount", &automount_name);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate unit name: %m");

                automount_unit = strjoin(arg_dest, "/", automount_name, NULL);
                if (!automount_unit)
                        return log_oom();

                fclose(f);
                f = fopen(automount_unit, "wxe");
                if (!f)
                        return log_error_errno(errno, "Failed to create unit file %s: %m", automount_unit);

                fprintf(f,
                        "# Automatically generated by systemd-fstab-generator\n\n"
                        "[Unit]\n"
                        "SourcePath=%s\n"
                        "Documentation=man:fstab(5) man:systemd-fstab-generator(8)\n",
                        source);

                fprintf(f, "Before=%s\n", post);

                if (opts) {
                        r = write_requires_after(f, opts);
                        if (r < 0)
                                return r;
                        r = write_requires_mounts_for(f, opts);
                        if (r < 0)
                                return r;
                }

                fprintf(f,
                        "\n"
                        "[Automount]\n"
                        "Where=%s\n",
                        where);

                r = write_idle_timeout(f, where, opts);
                if (r < 0)
                        return r;

                r = fflush_and_check(f);
                if (r < 0)
                        return log_error_errno(r, "Failed to write unit file %s: %m", automount_unit);

                free(lnk);
                lnk = strjoin(arg_dest, "/", post, nofail ? ".wants/" : ".requires/", automount_name, NULL);
                if (!lnk)
                        return log_oom();

                mkdir_parents_label(lnk, 0755);
                if (symlink(automount_unit, lnk) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);
        }

        return 0;
}

static int parse_fstab(bool initrd) {
        _cleanup_endmntent_ FILE *f = NULL;
        const char *fstab_path;
        struct mntent *me;
        int r = 0;

        fstab_path = initrd ? "/sysroot/etc/fstab" : "/etc/fstab";
        f = setmntent(fstab_path, "re");
        if (!f) {
                if (errno == ENOENT)
                        return 0;

                log_error_errno(errno, "Failed to open %s: %m", fstab_path);
                return -errno;
        }

        while ((me = getmntent(f))) {
                _cleanup_free_ char *where = NULL, *what = NULL;
                bool noauto, nofail;
                int k;

                if (initrd && !mount_in_initrd(me))
                        continue;

                what = fstab_node_to_udev_node(me->mnt_fsname);
                if (!what)
                        return log_oom();

                if (is_device_path(what) && path_is_read_only_fs("sys") > 0) {
                        log_info("Running in a container, ignoring fstab device entry for %s.", what);
                        continue;
                }

                where = initrd ? strappend("/sysroot/", me->mnt_dir) : strdup(me->mnt_dir);
                if (!where)
                        return log_oom();

                if (is_path(where))
                        path_kill_slashes(where);

                noauto = fstab_test_yes_no_option(me->mnt_opts, "noauto\0" "auto\0");
                nofail = fstab_test_yes_no_option(me->mnt_opts, "nofail\0" "fail\0");
                log_debug("Found entry what=%s where=%s type=%s nofail=%s noauto=%s",
                          what, where, me->mnt_type,
                          yes_no(noauto), yes_no(nofail));

                if (streq(me->mnt_type, "swap"))
                        k = add_swap(what, me, noauto, nofail);
                else {
                        bool automount;
                        const char *post;

                        automount = fstab_test_option(me->mnt_opts,
                                                      "comment=systemd.automount\0"
                                                      "x-systemd.automount\0");
                        if (initrd)
                                post = SPECIAL_INITRD_FS_TARGET;
                        else if (mount_is_network(me))
                                post = SPECIAL_REMOTE_FS_TARGET;
                        else
                                post = SPECIAL_LOCAL_FS_TARGET;

                        k = add_mount(what,
                                      where,
                                      me->mnt_type,
                                      me->mnt_opts,
                                      me->mnt_passno,
                                      noauto,
                                      nofail,
                                      automount,
                                      post,
                                      fstab_path);
                }

                if (k < 0)
                        r = k;
        }

        return r;
}

static int add_sysroot_mount(void) {
        _cleanup_free_ char *what = NULL;
        const char *opts;
        int r;

        if (isempty(arg_root_what)) {
                log_debug("Could not find a root= entry on the kernel command line.");
                return 0;
        }

        if (streq(arg_root_what, "gpt-auto")) {
                /* This is handled by the gpt-auto generator */
                log_debug("Skipping root directory handling, as gpt-auto was requested.");
                return 0;
        }

        if (streq(arg_root_what, "/dev/nfs")) {
                /* This is handled by the kernel or the initrd */
                log_debug("Skipping root directory handling, as /dev/nfs was requested.");
                return 0;
        }

        what = fstab_node_to_udev_node(arg_root_what);
        if (!what)
                return log_oom();

        if (!arg_root_options)
                opts = arg_root_rw > 0 ? "rw" : "ro";
        else if (arg_root_rw >= 0 ||
                 !fstab_test_option(arg_root_options, "ro\0" "rw\0"))
                opts = strjoina(arg_root_options, ",", arg_root_rw > 0 ? "rw" : "ro");
        else
                opts = arg_root_options;

        log_debug("Found entry what=%s where=/sysroot type=%s", what, strna(arg_root_fstype));

        if (is_device_path(what)) {
                r = generator_write_initrd_root_device_deps(arg_dest, what);
                if (r < 0)
                        return r;
        }

        return add_mount(what,
                         "/sysroot",
                         arg_root_fstype,
                         opts,
                         is_device_path(what) ? 1 : 0,
                         false,
                         false,
                         false,
                         SPECIAL_INITRD_ROOT_FS_TARGET,
                         "/proc/cmdline");
}

static int add_sysroot_usr_mount(void) {
        _cleanup_free_ char *what = NULL;
        const char *opts;

        if (!arg_usr_what && !arg_usr_fstype && !arg_usr_options)
                return 0;

        if (arg_root_what && !arg_usr_what) {
                arg_usr_what = strdup(arg_root_what);

                if (!arg_usr_what)
                        return log_oom();
        }

        if (arg_root_fstype && !arg_usr_fstype) {
                arg_usr_fstype = strdup(arg_root_fstype);

                if (!arg_usr_fstype)
                        return log_oom();
        }

        if (arg_root_options && !arg_usr_options) {
                arg_usr_options = strdup(arg_root_options);

                if (!arg_usr_options)
                        return log_oom();
        }

        if (!arg_usr_what)
                return 0;

        what = fstab_node_to_udev_node(arg_usr_what);
        if (!path_is_absolute(what)) {
                log_debug("Skipping entry what=%s where=/sysroot/usr type=%s", what, strna(arg_usr_fstype));
                return -1;
        }

        if (!arg_usr_options)
                opts = arg_root_rw > 0 ? "rw" : "ro";
        else if (!fstab_test_option(arg_usr_options, "ro\0" "rw\0"))
                opts = strjoina(arg_usr_options, ",", arg_root_rw > 0 ? "rw" : "ro");
        else
                opts = arg_usr_options;

        log_debug("Found entry what=%s where=/sysroot/usr type=%s", what, strna(arg_usr_fstype));
        return add_mount(what,
                         "/sysroot/usr",
                         arg_usr_fstype,
                         opts,
                         1,
                         false,
                         false,
                         false,
                         SPECIAL_INITRD_FS_TARGET,
                         "/proc/cmdline");
}

static int parse_proc_cmdline_item(const char *key, const char *value) {
        int r;

        /* root=, usr=, usrfstype= and roofstype= may occur more than once, the last
         * instance should take precedence.  In the case of multiple rootflags=
         * or usrflags= the arguments should be concatenated */

        if (STR_IN_SET(key, "fstab", "rd.fstab") && value) {

                r = parse_boolean(value);
                if (r < 0)
                        log_warning("Failed to parse fstab switch %s. Ignoring.", value);
                else
                        arg_fstab_enabled = r;

        } else if (streq(key, "root") && value) {

                if (free_and_strdup(&arg_root_what, value) < 0)
                        return log_oom();

        } else if (streq(key, "rootfstype") && value) {

                if (free_and_strdup(&arg_root_fstype, value) < 0)
                        return log_oom();

        } else if (streq(key, "rootflags") && value) {
                char *o;

                o = arg_root_options ?
                        strjoin(arg_root_options, ",", value, NULL) :
                        strdup(value);
                if (!o)
                        return log_oom();

                free(arg_root_options);
                arg_root_options = o;

        } else if (streq(key, "mount.usr") && value) {

                if (free_and_strdup(&arg_usr_what, value) < 0)
                        return log_oom();

        } else if (streq(key, "mount.usrfstype") && value) {

                if (free_and_strdup(&arg_usr_fstype, value) < 0)
                        return log_oom();

        } else if (streq(key, "mount.usrflags") && value) {
                char *o;

                o = arg_usr_options ?
                        strjoin(arg_usr_options, ",", value, NULL) :
                        strdup(value);
                if (!o)
                        return log_oom();

                free(arg_usr_options);
                arg_usr_options = o;

        } else if (streq(key, "rw") && !value)
                arg_root_rw = true;
        else if (streq(key, "ro") && !value)
                arg_root_rw = false;

        return 0;
}

int main(int argc, char *argv[]) {
        int r = 0;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[1];

        log_set_target(LOG_TARGET_SAFE);
        log_parse_environment();
        log_open();

        umask(0022);

        r = parse_proc_cmdline(parse_proc_cmdline_item);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        /* Always honour root= and usr= in the kernel command line if we are in an initrd */
        if (in_initrd()) {
                r = add_sysroot_mount();
                if (r == 0)
                        r = add_sysroot_usr_mount();
        }

        /* Honour /etc/fstab only when that's enabled */
        if (arg_fstab_enabled) {
                int k;

                log_debug("Parsing /etc/fstab");

                /* Parse the local /etc/fstab, possibly from the initrd */
                k = parse_fstab(false);
                if (k < 0)
                        r = k;

                /* If running in the initrd also parse the /etc/fstab from the host */
                if (in_initrd()) {
                        log_debug("Parsing /sysroot/etc/fstab");

                        k = parse_fstab(true);
                        if (k < 0)
                                r = k;
                }
        }

        free(arg_root_what);
        free(arg_root_fstype);
        free(arg_root_options);

        free(arg_usr_what);
        free(arg_usr_fstype);
        free(arg_usr_options);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
