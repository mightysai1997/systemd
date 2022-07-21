/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/nl80211.h>

#include "device-private.h"
#include "device-util.h"
#include "networkd-manager.h"
#include "networkd-wiphy.h"
#include "parse-util.h"
#include "udev-util.h"
#include "wifi-util.h"

Wiphy *wiphy_free(Wiphy *w) {
        if (!w)
                return NULL;

        if (w->manager) {
                hashmap_remove_value(w->manager->wiphy_by_index, UINT32_TO_PTR(w->index), w);
                if (w->name)
                        hashmap_remove_value(w->manager->wiphy_by_name, w->name, w);
        }

        sd_device_unref(w->dev);

        free(w->name);
        return mfree(w);
}

static int wiphy_new(Manager *manager, uint32_t index, Wiphy **ret) {
        _cleanup_(wiphy_freep) Wiphy *w = NULL;
        int r;

        assert(manager);

        w = new(Wiphy, 1);
        if (!w)
                return -ENOMEM;

        *w = (Wiphy) {
                .index = index,
        };

        r = hashmap_ensure_put(&manager->wiphy_by_index, NULL, UINT32_TO_PTR(w->index), w);
        if (r < 0)
                return r;

        w->manager = manager;

        if (ret)
                *ret = w;

        TAKE_PTR(w);
        return 0;
}

int wiphy_get_by_index(Manager *manager, uint32_t index, Wiphy **ret) {
        Wiphy *w;

        assert(manager);

        w = hashmap_get(manager->wiphy_by_index, UINT32_TO_PTR(index));
        if (!w)
                return -ENODEV;

        if (ret)
                *ret = w;

        return 0;
}

int wiphy_get_by_name(Manager *manager, const char *name, Wiphy **ret) {
        Wiphy *w;

        assert(manager);
        assert(name);

        w = hashmap_get(manager->wiphy_by_name, name);
        if (!w)
                return -ENODEV;

        if (ret)
                *ret = w;

        return 0;
}

static int wiphy_update_name(Wiphy *w, sd_netlink_message *message) {
        const char *name;
        int r;

        assert(w);
        assert(w->manager);
        assert(message);

        r = sd_netlink_message_read_string(message, NL80211_ATTR_WIPHY_NAME, &name);
        if (r == -ENODATA)
                return 0;
        if (r < 0)
                return r;

        if (streq_ptr(w->name, name))
                return 0;

        if (w->name)
                hashmap_remove_value(w->manager->wiphy_by_name, w->name, w);

        r = free_and_strdup(&w->name, name);
        if (r < 0)
                return r;

        r = hashmap_ensure_put(&w->manager->wiphy_by_name, &string_hash_ops, w->name, w);
        if (r < 0)
                return r;

        return 1;
}

static int wiphy_update_device(Wiphy *w) {
        _cleanup_(sd_device_unrefp) sd_device *dev = NULL;
        int r;

        assert(w);

        if (!udev_available())
                return 0;

        w->dev = sd_device_unref(w->dev);

        if (!w->name)
                return 0;

        /* The corresponding syspath may not exist yet. */
        r = sd_device_new_from_subsystem_sysname(&dev, "ieee80211", w->name);
        if (r < 0) {
                log_wiphy_debug_errno(w, r, "Failed to get wiphy device, ignoring: %m");
                return 0;
        }

        w->dev = TAKE_PTR(dev);
        return 0;
}

static int wiphy_update(Wiphy *w, sd_netlink_message *message) {
        int r;

        assert(w);
        assert(message);

        r = wiphy_update_name(w, message);
        if (r < 0)
                return log_wiphy_debug_errno(w, r, "Failed to update wiphy name: %m");
        if (r == 0)
                return 0;

        r = wiphy_update_device(w);
        if (r < 0)
                return log_wiphy_debug_errno(w, r, "Failed to update wiphy device: %m");

        return 0;
}

int manager_genl_process_nl80211_wiphy(sd_netlink *genl, sd_netlink_message *message, Manager *manager) {
        const char *family;
        uint32_t index;
        uint8_t cmd;
        Wiphy *w = NULL;
        int r;

        assert(genl);
        assert(message);
        assert(manager);

        if (sd_netlink_message_is_error(message)) {
                r = sd_netlink_message_get_errno(message);
                if (r < 0)
                        log_message_warning_errno(message, r, "nl80211: received error message, ignoring");

                return 0;
        }

        r = sd_genl_message_get_family_name(genl, message, &family);
        if (r < 0) {
                log_debug_errno(r, "nl80211: failed to determine genl family, ignoring: %m");
                return 0;
        }
        if (!streq(family, NL80211_GENL_NAME)) {
                log_debug("nl80211: Received message of unexpected genl family '%s', ignoring.", family);
                return 0;
        }

        r = sd_genl_message_get_command(genl, message, &cmd);
        if (r < 0) {
                log_debug_errno(r, "nl80211: failed to determine genl message command, ignoring: %m");
                return 0;
        }

        r = sd_netlink_message_read_u32(message, NL80211_ATTR_WIPHY, &index);
        if (r < 0) {
                log_debug_errno(r, "nl80211: received %s(%u) message without valid index, ignoring: %m",
                                strna(nl80211_cmd_to_string(cmd)), cmd);
                return 0;
        }

        (void) wiphy_get_by_index(manager, index, &w);

        switch (cmd) {
        case NL80211_CMD_NEW_WIPHY: {
                bool is_new = !w;

                if (!w) {
                        r = wiphy_new(manager, index, &w);
                        if (r < 0) {
                                log_warning_errno(r, "Failed to allocate wiphy, ignoring: %m");
                                return 0;
                        }
                }

                r = wiphy_update(w, message);
                if (r < 0) {
                        log_wiphy_warning_errno(w, r, "Failed to update wiphy, ignoring: %m");
                        return 0;
                }

                log_wiphy_debug(w, "Received %s phy.", is_new ? "new" : "updated");
                break;
        }
        case NL80211_CMD_DEL_WIPHY:

                if (!w) {
                        log_debug("The kernel removes wiphy we do not know, ignoring: %m");
                        return 0;
                }

                log_wiphy_debug(w, "Removed.");
                wiphy_free(w);
                break;

        default:
                log_wiphy_debug(w, "nl80211: received %s(%u) message.",
                                strna(nl80211_cmd_to_string(cmd)), cmd);
        }

        return 0;
}

int manager_udev_process_wiphy(Manager *m, sd_device *device, sd_device_action_t action) {
        const char *name;
        Wiphy *w;
        int r;

        assert(m);
        assert(device);

        r = sd_device_get_sysname(device, &name);
        if (r < 0)
                return log_device_debug_errno(device, r, "Failed to get sysname: %m");

        r = wiphy_get_by_name(m, name, &w);
        if (r < 0) {
                /* This error is not critical, as the corresponding genl message may be received later. */
                log_device_debug_errno(device, r, "Failed to get Wiphy object, ignoring: %m");
                return 0;
        }

        return device_unref_and_replace(w->dev, action == SD_DEVICE_REMOVE ? NULL : device);
}
