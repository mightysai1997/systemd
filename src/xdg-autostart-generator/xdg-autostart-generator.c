/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include "dirent-util.h"
#include "fd-util.h"
#include "generator.h"
#include "hashmap.h"
#include "log.h"
#include "main-func.h"
#include "nulstr-util.h"
#include "path-lookup.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "xdg-autostart-service.h"

DEFINE_PRIVATE_HASH_OPS_WITH_VALUE_DESTRUCTOR(xdgautostartservice_hash_ops, char, string_hash_func, string_compare_func, XdgAutostartService, xdg_autostart_service_free);

static int enumerate_xdg_autostart(Hashmap *all_services) {
        _cleanup_strv_free_ char **autostart_dirs = NULL;
        _cleanup_strv_free_ char **config_dirs = NULL;
        _unused_ _cleanup_strv_free_ char **data_dirs = NULL;
        _cleanup_free_ char *user_config_autostart_dir = NULL;
        char **path;
        int r;

        r = xdg_user_config_dir(&user_config_autostart_dir, "/autostart");
        if (r < 0)
                return r;
        r = strv_extend(&autostart_dirs, user_config_autostart_dir);
        if (r < 0)
                return r;

        r = xdg_user_dirs(&config_dirs, &data_dirs);
        if (r < 0)
                return r;
        r = strv_extend_strv_concat(&autostart_dirs, config_dirs, "/autostart");
        if (r < 0)
                return r;

        STRV_FOREACH(path, autostart_dirs) {
                _cleanup_closedir_ DIR *d = NULL;

                d = opendir(*path);
                if (!d) {
                        if (errno != ENOENT)
                                log_warning_errno(errno, "Opening %s failed, ignoring: %m", *path);
                        continue;
                }

                FOREACH_DIRENT(de, d, log_warning_errno(errno, "Failed to enumerate directory %s, ignoring: %m", *path)) {
                        _cleanup_free_ char *fpath = NULL, *name = NULL;
                        _cleanup_(xdg_autostart_service_freep) XdgAutostartService *service = NULL;
                        struct stat st;

                        if (fstatat(dirfd(d), de->d_name, &st, 0) < 0) {
                                log_warning_errno(errno, "stat() failed on %s/%s, ignoring: %m", *path, de->d_name);
                                continue;
                        }

                        if (!S_ISREG(st.st_mode))
                                continue;

                        name = xdg_autostart_service_translate_name(de->d_name);
                        if (!name)
                                return log_oom();

                        if (hashmap_contains(all_services, name))
                                continue;

                        fpath = path_join(*path, de->d_name);
                        if (!fpath)
                                return log_oom();

                        service = xdg_autostart_service_parse_desktop(fpath);
                        if (!service)
                                return log_oom();
                        service->name = TAKE_PTR(name);

                        r = hashmap_put(all_services, service->name, service);
                        if (r < 0)
                                return log_oom();
                        TAKE_PTR(service);
                }
        }

        return 0;
}

static int run(const char *dest, const char *dest_early, const char *dest_late) {
        _cleanup_(hashmap_freep) Hashmap *all_services = NULL;
        XdgAutostartService *service;
        int r;

        assert_se(dest_late);

        all_services = hashmap_new(&xdgautostartservice_hash_ops);
        if (!all_services)
                return log_oom();

        r = enumerate_xdg_autostart(all_services);
        if (r < 0)
                return r;

        HASHMAP_FOREACH(service, all_services)
                (void) xdg_autostart_service_generate_unit(service, dest_late);

        return 0;
}

DEFINE_MAIN_GENERATOR_FUNCTION(run);
