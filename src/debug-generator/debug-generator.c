/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "alloc-util.h"
#include "creds-util.h"
#include "dropin.h"
#include "errno-util.h"
#include "fd-util.h"
#include "fileio-label.h"
#include "generator.h"
#include "initrd-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "recurse-dir.h"
#include "special.h"
#include "string-util.h"
#include "strv.h"
#include "unit-file.h"
#include "unit-name.h"

static const char *arg_dest = NULL;
static char *arg_default_unit = NULL;
static char **arg_mask = NULL;
static char **arg_wants = NULL;
static bool arg_debug_shell = false;
static char *arg_debug_tty = NULL;
static char *arg_default_debug_tty = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_default_unit, freep);
STATIC_DESTRUCTOR_REGISTER(arg_mask, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_wants, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_debug_tty, freep);
STATIC_DESTRUCTOR_REGISTER(arg_default_debug_tty, freep);

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        assert(key);

        if (streq(key, "systemd.mask")) {
                char *n;

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = unit_name_mangle(value, UNIT_NAME_MANGLE_WARN, &n);
                if (r < 0)
                        return log_error_errno(r, "Failed to glob unit name: %m");

                r = strv_consume(&arg_mask, n);
                if (r < 0)
                        return log_oom();

        } else if (streq(key, "systemd.wants")) {
                char *n;

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = unit_name_mangle(value, UNIT_NAME_MANGLE_WARN, &n);
                if (r < 0)
                        return log_error_errno(r, "Failed to glob unit name: %m");

                r = strv_consume(&arg_wants, n);
                if (r < 0)
                        return log_oom();

        } else if (proc_cmdline_key_streq(key, "systemd.debug_shell")) {
                r = value ? parse_boolean(value) : 1;
                arg_debug_shell = r != 0;
                if (r >= 0)
                        return 0;

                return free_and_strdup_warn(&arg_debug_tty, skip_dev_prefix(value));

        } else if (proc_cmdline_key_streq(key, "systemd.default_debug_tty")) {
                if (proc_cmdline_value_missing(key, value))
                        return 0;

                return free_and_strdup_warn(&arg_default_debug_tty, skip_dev_prefix(value));

        } else if (streq(key, "systemd.unit")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                return free_and_strdup_warn(&arg_default_unit, value);

        } else if (!value) {
                const char *target;

                target = runlevel_to_target(key);
                if (target)
                        return free_and_strdup_warn(&arg_default_unit, target);
        }

        return 0;
}

static int generate_mask_symlinks(void) {
        int r = 0;

        STRV_FOREACH(u, arg_mask) {
                _cleanup_free_ char *p = NULL;

                p = path_join(empty_to_root(arg_dest), *u);
                if (!p)
                        return log_oom();

                if (symlink("/dev/null", p) < 0)
                        r = log_error_errno(errno,
                                            "Failed to create mask symlink %s: %m",
                                            p);
        }

        return r;
}

static int generate_wants_symlinks(void) {
        int r = 0;

        STRV_FOREACH(u, arg_wants) {
                _cleanup_free_ char *f = NULL;
                const char *target;

                /* This should match what do_queue_default_job() in core/main.c does. */
                if (arg_default_unit)
                        target = arg_default_unit;
                else if (in_initrd())
                        target = SPECIAL_INITRD_TARGET;
                else
                        target = SPECIAL_DEFAULT_TARGET;

                f = path_join(SYSTEM_DATA_UNIT_DIR, *u);
                if (!f)
                        return log_oom();

                r = generator_add_symlink(arg_dest, target, "wants", f);
                if (r < 0)
                        return r;
        }

        return r;
}

static void install_debug_shell_dropin(void) {
        const char *tty = arg_debug_tty ?: arg_default_debug_tty;
        int r;

        if (!tty || path_equal(tty, skip_dev_prefix(DEBUGTTY)))
                return;

        r = write_drop_in_format(arg_dest, "debug-shell.service", 50, "tty",
                        "[Unit]\n"
                        "Description=Early root shell on /dev/%s FOR DEBUGGING ONLY\n"
                        "ConditionPathExists=\n"
                        "[Service]\n"
                        "TTYPath=/dev/%s",
                        tty, tty);
        if (r < 0)
                log_warning_errno(r, "Failed to write drop-in for debug-shell.service, ignoring: %m");
}

static int process_unit_credentials(const char *credentials_dir) {
        _cleanup_close_ int fd = -EBADF;
        int r;

        assert(credentials_dir);

        fd = open(credentials_dir, O_CLOEXEC|O_DIRECTORY);
        if (fd < 0) {
                if (errno == ENOENT)
                        return 0;

                return log_error_errno(errno, "Failed to open credentials directory '%s': %m", credentials_dir);
        }

        _cleanup_free_ DirectoryEntries *des = NULL;
        r = readdir_all(fd, RECURSE_DIR_SORT|RECURSE_DIR_IGNORE_DOT|RECURSE_DIR_ENSURE_TYPE, &des);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate credentials from credentials directory '%s': %m", credentials_dir);

        FOREACH_ARRAY(i, des->entries, des->n_entries) {
                _cleanup_free_ void *d = NULL;
                struct dirent *de = *i;
                const char *unit, *dropin;

                if (de->d_type != DT_REG)
                        continue;

                unit = startswith(de->d_name, "systemd.extra-unit.");
                dropin = startswith(de->d_name, "systemd.unit-dropin.");

                if (!unit && !dropin)
                        continue;

                if (!unit_name_is_valid(unit ?: dropin, UNIT_NAME_ANY)) {
                        log_warning_errno(SYNTHETIC_ERRNO(EINVAL),
                                          "Invalid unit name '%s' in credential '%s', ignoring.",
                                          unit ?: dropin, de->d_name);
                        continue;
                }

                r = read_credential_with_decryption(de->d_name, &d, NULL);
                if (r < 0)
                        continue;

                if (unit) {
                        _cleanup_free_ char *p = NULL;

                        p = path_join(arg_dest, unit);
                        if (!p)
                                return log_oom();

                        r = write_string_file_atomic_label(p, d);
                        if (r < 0) {
                                log_warning_errno(r, "Failed to write unit file '%s' from credential '%s', ignoring: %m", unit, de->d_name);
                                continue;
                        }

                        log_debug("Wrote unit file '%s' from credential '%s'", unit, de->d_name);

                } else {
                        r = write_drop_in(arg_dest, dropin, 50, "credential", d);
                        if (r < 0) {
                                log_warning_errno(r, "Failed to write drop-in for unit '%s' from credential '%s', ignoring: %m", dropin, de->d_name);
                                continue;
                        }

                        log_debug("Wrote drop-in for unit '%s' from credential '%s'", dropin, de->d_name);
                }
        }

        return 0;
}

static int run(const char *dest, const char *dest_early, const char *dest_late) {
        const char *credentials_dir;
        int r = 0;

        assert_se(arg_dest = dest_early);

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, PROC_CMDLINE_RD_STRICT | PROC_CMDLINE_STRIP_RD_PREFIX);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (arg_debug_shell) {
                r = strv_extend(&arg_wants, "debug-shell.service");
                if (r < 0)
                        return log_oom();

                install_debug_shell_dropin();
        }

        if (get_credentials_dir(&credentials_dir) >= 0)
                RET_GATHER(r, process_unit_credentials(credentials_dir));

        if (get_encrypted_credentials_dir(&credentials_dir) >= 0)
                RET_GATHER(r, process_unit_credentials(credentials_dir));

        RET_GATHER(r, generate_mask_symlinks());
        RET_GATHER(r, generate_wants_symlinks());

        return r;
}

DEFINE_MAIN_GENERATOR_FUNCTION(run);
