/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdio.h>
#include <string.h>

#include "install.h"
#include "tests.h"

typedef struct MacUnitCallbackUserdata {
        const char *function;
        const char *selinux_permission;
} MacUnitCallbackUserdata;

static void dump_changes(InstallChange *c, unsigned n) {
        unsigned i;

        assert_se(n == 0 || c);

        for (i = 0; i < n; i++) {
                if (c[i].type == INSTALL_CHANGE_UNLINK)
                        printf("rm '%s'\n", c[i].path);
                else if (c[i].type == INSTALL_CHANGE_SYMLINK)
                        printf("ln -s '%s' '%s'\n", c[i].source, c[i].path);
        }
}

static unsigned mac_callback_called = 0;

static int mac_callback(const char *unit_name, void *userdata) {
        MacUnitCallbackUserdata *ud = userdata;

        assert_se(unit_name);
        assert_se(STR_IN_SET(unit_name, "avahi-daemon.service", "test.service"));
        assert_se(ud);

        assert_se(strstr(ud->function, "main"));

        mac_callback_called++;

        return STR_IN_SET(ud->selinux_permission, "start", "stop", "status", "reload", "enable", "disable") ? 0 : -3141;
}

int main(int argc, char* argv[]) {
        Hashmap *h;
        UnitFileList *p;
        int r;
        const char *const files[] = { "avahi-daemon.service", NULL };
        const char *const files2[] = { "/home/lennart/test.service", NULL };
        InstallChange *changes = NULL;
        size_t n_changes = 0;
        UnitFileState state = 0;
        MacUnitCallbackUserdata mcud = {
                .function = __func__,
                .selinux_permission = "!INVALID!",
        };
        unsigned expect_called = 0;

        test_setup_logging(LOG_DEBUG);

        assert_se(mac_callback_called == expect_called);

        h = hashmap_new(&string_hash_ops);
        r = unit_file_get_list(LOOKUP_SCOPE_SYSTEM, NULL, h, NULL, NULL);
        assert_se(r == 0);

        HASHMAP_FOREACH(p, h) {
                UnitFileState s = _UNIT_FILE_STATE_INVALID;

                r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, basename(p->path), &s);

                assert_se((r < 0 && p->state == UNIT_FILE_BAD) ||
                          (p->state == s));

                fprintf(stderr, "%s (%s)\n",
                        p->path,
                        unit_file_state_to_string(p->state));
        }

        unit_file_list_free(h);

        log_info("/*** enable **/");

        r = unit_file_enable(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r == -3141);
        assert_se(mac_callback_called == ++expect_called);

        mcud.selinux_permission = "enable";
        r = unit_file_enable(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        log_info("/*** enable2 **/");

        r = unit_file_enable(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, files[0], &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_ENABLED);

        log_info("/*** disable ***/");
        changes = NULL;
        n_changes = 0;

        mcud.selinux_permission = "disable";
        r = unit_file_disable(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, files[0], &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_DISABLED);

        log_info("/*** mask ***/");
        changes = NULL;
        n_changes = 0;

        mcud.selinux_permission = "disable";
        r = unit_file_mask(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);
        log_info("/*** mask2 ***/");
        r = unit_file_mask(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, files[0], &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_MASKED);

        log_info("/*** unmask ***/");
        changes = NULL;
        n_changes = 0;

        mcud.selinux_permission = "enable";
        r = unit_file_unmask(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);
        log_info("/*** unmask2 ***/");
        r = unit_file_unmask(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, files[0], &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_DISABLED);

        log_info("/*** mask ***/");
        changes = NULL;
        n_changes = 0;

        mcud.selinux_permission = "disable";
        r = unit_file_mask(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, files[0], &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_MASKED);

        log_info("/*** disable ***/");
        changes = NULL;
        n_changes = 0;

        mcud.selinux_permission = "disable";
        r = unit_file_disable(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);
        log_info("/*** disable2 ***/");
        r = unit_file_disable(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, files[0], &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_MASKED);

        log_info("/*** umask ***/");
        changes = NULL;
        n_changes = 0;

        mcud.selinux_permission = "enable";
        r = unit_file_unmask(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, files[0], &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_DISABLED);

        log_info("/*** enable files2 ***/");
        changes = NULL;
        n_changes = 0;

        r = unit_file_enable(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files2, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, basename(files2[0]), &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_ENABLED);

        log_info("/*** disable files2 ***/");
        changes = NULL;
        n_changes = 0;

        mcud.selinux_permission = "disable";
        r = unit_file_disable(LOOKUP_SCOPE_SYSTEM, 0, NULL, STRV_MAKE(basename(files2[0])), &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, basename(files2[0]), &state);
        assert_se(r < 0);

        log_info("/*** link files2 ***/");
        changes = NULL;
        n_changes = 0;

        r = unit_file_link(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files2, &changes, &n_changes, NULL, NULL);
        assert_se(r >= 0);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, basename(files2[0]), &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_LINKED);

        log_info("/*** disable files2 ***/");
        changes = NULL;
        n_changes = 0;

        r = unit_file_disable(LOOKUP_SCOPE_SYSTEM, 0, NULL, STRV_MAKE(basename(files2[0])), &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, basename(files2[0]), &state);
        assert_se(r < 0);

        log_info("/*** link files2 ***/");
        changes = NULL;
        n_changes = 0;

        r = unit_file_link(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files2, &changes, &n_changes, NULL, NULL);
        assert_se(r >= 0);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, basename(files2[0]), &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_LINKED);

        log_info("/*** reenable files2 ***/");
        changes = NULL;
        n_changes = 0;

        mcud.selinux_permission = "enable";
        r = unit_file_reenable(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files2, &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == (expect_called += 2)); /* disable and enable */

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, basename(files2[0]), &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_ENABLED);

        log_info("/*** disable files2 ***/");
        changes = NULL;
        n_changes = 0;

        mcud.selinux_permission = "disable";
        r = unit_file_disable(LOOKUP_SCOPE_SYSTEM, 0, NULL, STRV_MAKE(basename(files2[0])), &changes, &n_changes, mac_callback, &mcud);
        assert_se(r >= 0);
        assert_se(mac_callback_called == ++expect_called);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, basename(files2[0]), &state);
        assert_se(r < 0);
        log_info("/*** preset files ***/");
        changes = NULL;
        n_changes = 0;

        r = unit_file_preset(LOOKUP_SCOPE_SYSTEM, 0, NULL, (char**) files, UNIT_FILE_PRESET_FULL, &changes, &n_changes, NULL, NULL);
        assert_se(r >= 0);

        dump_changes(changes, n_changes);
        install_changes_free(changes, n_changes);

        r = unit_file_get_state(LOOKUP_SCOPE_SYSTEM, NULL, basename(files[0]), &state);
        assert_se(r >= 0);
        assert_se(state == UNIT_FILE_ENABLED);

        return 0;
}
