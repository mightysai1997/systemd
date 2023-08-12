/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <unistd.h>

#include "alloc-util.h"
#include "escape.h"
#include "fd-util.h"
#include "fileio.h"
#include "generator.h"
#include "glyph-util.h"
#include "mkdir.h"
#include "proc-cmdline.h"
#include "special.h"
#include "specifier.h"
#include "strv.h"

static const char *arg_dest = NULL;
static char **arg_commands = NULL;
static char *arg_success_action = NULL;
static char *arg_failure_action = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_commands, strv_freep);
STATIC_DESTRUCTOR_REGISTER(arg_success_action, freep);
STATIC_DESTRUCTOR_REGISTER(arg_failure_action, freep);

static int parse(const char *key, const char *value, void *data) {
        int r;

        assert(key);

        if (streq(key, "systemd.run")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                r = strv_extend(&arg_commands, value);
                if (r < 0)
                        return log_oom();

        } else if (proc_cmdline_key_streq(key, "systemd.run_success_action")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                return free_and_strdup_warn(&arg_success_action, value);

        } else if (proc_cmdline_key_streq(key, "systemd.run_failure_action")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                return free_and_strdup_warn(&arg_failure_action, value);
        }

        return 0;
}

static int generate(void) {
        _cleanup_fclose_ FILE *f = NULL;
        const char *p;
        int r;

        if (strv_isempty(arg_commands) && !arg_success_action)
                return 0;

        p = strjoina(arg_dest, "/kernel-command-line.service");
        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", p);

        fputs("# Automatically generated by systemd-run-generator\n\n"
              "[Unit]\n"
              "Description=Command from Kernel Command Line\n"
              "Documentation=man:systemd-run-generator(8)\n"
              "SourcePath=/proc/cmdline\n", f);

        if (!streq_ptr(arg_success_action, "none"))
                fprintf(f, "SuccessAction=%s\n",
                        arg_success_action ?: "exit");

        if (!streq_ptr(arg_failure_action, "none"))
                fprintf(f, "FailureAction=%s\n",
                        arg_failure_action ?: "exit");

        fputs("\n"
              "[Service]\n"
              "Type=oneshot\n"
              "StandardOutput=journal+console\n", f);

        STRV_FOREACH(c, arg_commands) {
                _cleanup_free_ char *a = NULL;

                /* The kernel cmdline can't contain special characters (we discard them during parsing). So
                 * unit_setting_escape_path is not necessary. */
                a = specifier_escape(*c);
                if (!a)
                        return log_oom();

                fprintf(f, "ExecStart=%s\n", a);
        }

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        /* Let's create a target we can link "default.target" to */
        p = strjoina(arg_dest, "/kernel-command-line.target");
        r = write_string_file(
                        p,
                        "# Automatically generated by systemd-run-generator\n\n"
                        "[Unit]\n"
                        "Description=Command from Kernel Command Line\n"
                        "Documentation=man:systemd-run-generator(8)\n"
                        "SourcePath=/proc/cmdline\n"
                        "Requires=kernel-command-line.service\n"
                        "After=kernel-command-line.service\n",
                        WRITE_STRING_FILE_CREATE|WRITE_STRING_FILE_NOFOLLOW);
        if (r < 0)
                return log_error_errno(r, "Failed to create unit file %s: %m", p);

        /* And now redirect default.target to our new target */
        p = strjoina(arg_dest, "/" SPECIAL_DEFAULT_TARGET);
        if (symlink("kernel-command-line.target", p) < 0)
                return log_error_errno(errno, "Failed to link unit file kernel-command-line.target %s %s: %m",
                                       special_glyph(SPECIAL_GLYPH_ARROW_RIGHT), p);

        return 0;
}

static int run(const char *dest, const char *dest_early, const char *dest_late) {
        int r;

        assert_se(arg_dest = dest);

        r = proc_cmdline_parse(parse, NULL, PROC_CMDLINE_RD_STRICT|PROC_CMDLINE_STRIP_RD_PREFIX);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        return generate();
}

DEFINE_MAIN_GENERATOR_FUNCTION(run);
