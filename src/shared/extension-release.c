/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "env-util.h"
#include "extension-release.h"
#include "log.h"
#include "os-util.h"
#include "strv.h"

int extension_release_validate(
                const char *name,
                const char *host_os_release_id,
                const char *host_os_release_version_id,
                const char *host_os_release_sysext_level,
                char **extension_release) {

        const char *extension_release_id = NULL, *extension_release_sysext_level = NULL;

        assert(name);
        assert(!isempty(host_os_release_id));

        /* Now that we can look into the extension image, let's see if the OS version is compatible */
        if (strv_isempty(extension_release)) {
                log_debug("Extension '%s' carries no extension-release data, ignoring extension.", name);
                return 0;
        }

        extension_release_id = strv_env_pairs_get(extension_release, "ID");
        if (isempty(extension_release_id)) {
                log_debug("Extension '%s' does not contain ID in extension-release but requested to match '%s'",
                          name, strna(host_os_release_id));
                return 0;
        }

        if (!streq_ptr(host_os_release_id, extension_release_id)) {
                log_debug("Extension '%s' is for OS '%s', but deployed on top of '%s'.",
                          name, strna(extension_release_id), strna(host_os_release_id));
                return 0;
        }

        /* Rolling releases do not typically set VERSION_ID (eg: ArchLinux) */
        if (isempty(host_os_release_version_id) && isempty(host_os_release_sysext_level)) {
                log_debug("No version info on the host (rolling release?), but ID in %s matched.", name);
                return 1;
        }

        /* If the extension has a sysext API level declared, then it must match the host API
         * level. Otherwise, compare OS version as a whole */
        extension_release_sysext_level = strv_env_pairs_get(extension_release, "SYSEXT_LEVEL");
        if (!isempty(host_os_release_sysext_level) && !isempty(extension_release_sysext_level)) {
                if (!streq_ptr(host_os_release_sysext_level, extension_release_sysext_level)) {
                        log_debug("Extension '%s' is for sysext API level '%s', but running on sysext API level '%s'",
                                  name, strna(extension_release_sysext_level), strna(host_os_release_sysext_level));
                        return 0;
                }
        } else if (!isempty(host_os_release_version_id)) {
                const char *extension_release_version_id;

                extension_release_version_id = strv_env_pairs_get(extension_release, "VERSION_ID");
                if (isempty(extension_release_version_id)) {
                        log_debug("Extension '%s' does not contain VERSION_ID in extension-release but requested to match '%s'",
                                  name, strna(host_os_release_version_id));
                        return 0;
                }

                if (!streq_ptr(host_os_release_version_id, extension_release_version_id)) {
                        log_debug("Extension '%s' is for OS '%s', but deployed on top of '%s'.",
                                  name, strna(extension_release_version_id), strna(host_os_release_version_id));
                        return 0;
                }
        } else if (isempty(host_os_release_version_id) && isempty(host_os_release_sysext_level)) {
                /* Rolling releases do not typically set VERSION_ID (eg: ArchLinux) */
                log_debug("No version info on the host (rolling release?), but ID in %s matched.", name);
                return 1;
        }

        log_debug("Version info of extension '%s' matches host.", name);
        return 1;
}

int parse_env_extension_hierarchies(char ***ret_hierarchies) {
        int r;

        r = getenv_path_list("SYSTEMD_SYSEXT_HIERARCHIES", ret_hierarchies);
         /* Ignore malformed variables, but propagate OOM */
        if (r == -ENOMEM)
                return r;
        if (r < 0) {
                log_debug_errno(r, "Failed to parse SYSTEMD_SYSEXT_HIERARCHIES environment variable: %m");
                return 0;
        }
        if (!*ret_hierarchies) {
                *ret_hierarchies = strv_new("/usr", "/opt");
                if (!*ret_hierarchies)
                        return -ENOMEM;
        }

        return 0;
}
