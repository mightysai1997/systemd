/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <netinet/in.h>
#include <linux/if.h>

#include "missing_network.h"
#include "netlink-util.h"
#include "networkd-link.h"
#include "networkd-manager.h"
#include "networkd-queue.h"
#include "string-table.h"

static const char *const set_link_mode_table[_SET_LINK_MODE_MAX] = {
        [SET_LINK_FLAGS] = "link flags",
        [SET_LINK_MTU] = "MTU",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP_TO_STRING(set_link_mode, SetLinkMode);

static int set_link_handler_internal(sd_netlink *rtnl, sd_netlink_message *m, Link *link, SetLinkMode mode, bool ignore) {
        int r;

        assert(m);
        assert(link);
        assert(link->set_link_messages > 0);
        assert(mode >= 0 && mode < _SET_LINK_MODE_MAX);

        link->set_link_messages--;

        if (IN_SET(link->state, LINK_STATE_FAILED, LINK_STATE_LINGER))
                return 0;

        r = sd_netlink_message_get_errno(m);
        if (r < 0) {
                const char *error_msg;

                error_msg = strjoina("Failed to set ", set_link_mode_to_string(mode), ignore ? ", ignoring" : "");
                log_link_message_warning_errno(link, m, r, error_msg);

                if (!ignore)
                        link_enter_failed(link);
                return 0;
        }

        return 1;
}

static int link_set_flags_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        return set_link_handler_internal(rtnl, m, link, SET_LINK_FLAGS, true);
}

static int link_set_mtu_handler(sd_netlink *rtnl, sd_netlink_message *m, Link *link) {
        int r;

        r = set_link_handler_internal(rtnl, m, link, SET_LINK_MTU, true);
        if (r <= 0)
                return r;

        /* The kernel resets ipv6 mtu after changing device mtu;
         * we must set this here, after we've set device mtu */
        r = link_set_ipv6_mtu(link);
        if (r < 0)
                log_link_warning_errno(link, r, "Failed to set IPv6 MTU, ignoring: %m");

        return 0;
}

static int link_configure(
                Link *link,
                SetLinkMode mode,
                void *userdata,
                link_netlink_message_handler_t callback) {

        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        int r;

        assert(link);
        assert(link->manager);
        assert(link->manager->rtnl);
        assert(link->network);
        assert(mode >= 0 && mode < _SET_LINK_MODE_MAX);
        assert(callback);

        log_link_debug(link, "Setting %s", set_link_mode_to_string(mode));

        r = sd_rtnl_message_new_link(link->manager->rtnl, &req, RTM_SETLINK, link->ifindex);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not allocate RTM_SETLINK message: %m");

        switch(mode) {
        case SET_LINK_FLAGS: {
                unsigned ifi_change = 0, ifi_flags = 0;

                if (link->network->arp >= 0) {
                        SET_FLAG(ifi_change, IFF_NOARP, true);
                        SET_FLAG(ifi_flags, IFF_NOARP, link->network->arp == 0);
                }

                if (link->network->multicast >= 0) {
                        SET_FLAG(ifi_change, IFF_MULTICAST, true);
                        SET_FLAG(ifi_flags, IFF_MULTICAST, link->network->multicast);
                }

                if (link->network->allmulticast >= 0) {
                        SET_FLAG(ifi_change, IFF_ALLMULTI, true);
                        SET_FLAG(ifi_flags, IFF_ALLMULTI, link->network->allmulticast);
                }

                if (link->network->promiscuous >= 0) {
                        SET_FLAG(ifi_change, IFF_PROMISC, true);
                        SET_FLAG(ifi_flags, IFF_PROMISC, link->network->promiscuous);
                }

                r = sd_rtnl_message_link_set_flags(req, ifi_flags, ifi_change);
                if (r < 0)
                        return log_link_debug_errno(link, r, "Could not set link flags: %m");

                break;
        }
        case SET_LINK_MTU:
                r = sd_netlink_message_append_u32(req, IFLA_MTU, PTR_TO_UINT32(userdata));
                if (r < 0)
                        return log_link_debug_errno(link, r, "Could not append IFLA_MTU attribute: %m");
                break;
        default:
                assert_not_reached("Invalid set link mode");
        }

        r = netlink_call_async(link->manager->rtnl, NULL, req, callback,
                               link_netlink_destroy_callback, link);
        if (r < 0)
                return log_link_debug_errno(link, r, "Could not send RTM_SETLINK message: %m");

        link_ref(link);
        return 0;
}

static bool link_is_ready_to_call_set_link(Link *link, SetLinkMode mode) {
        assert(link);

        if (!IN_SET(link->state, LINK_STATE_INITIALIZED, LINK_STATE_CONFIGURING, LINK_STATE_CONFIGURED))
                return false;

        return true;
}

int request_process_set_link(Request *req) {
        SetLinkMode mode;
        Link *link;
        int r;

        assert(req);
        assert(req->link);
        assert(req->type == REQUEST_TYPE_SET_LINK);
        assert(req->set_link_mode >= 0 && req->set_link_mode < _SET_LINK_MODE_MAX);
        assert(req->netlink_handler);

        link = req->link;
        mode = req->set_link_mode;

        if (!link_is_ready_to_call_set_link(link, mode))
                return 0;

        r = link_configure(link, mode, req->userdata, req->netlink_handler);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to set %s: %m", set_link_mode_to_string(mode));

        return 1;
}

static int link_request_set_link(
                Link *link,
                SetLinkMode mode,
                link_netlink_message_handler_t netlink_handler,
                Request **ret) {

        Request *req;
        int r;

        assert(link);
        assert(mode >= 0 && mode < _SET_LINK_MODE_MAX);
        assert(netlink_handler);

        r = link_queue_request(link, REQUEST_TYPE_SET_LINK, INT_TO_PTR(mode), false,
                               &link->set_link_messages, netlink_handler, &req);
        if (r < 0)
                return log_link_error_errno(link, r, "Failed to request to set %s: %m",
                                            set_link_mode_to_string(mode));

        log_link_debug(link, "Requested to set %s", set_link_mode_to_string(mode));

        if (ret)
                *ret = req;
        return 0;
}

int link_request_to_set_flags(Link *link) {
        assert(link);
        assert(link->network);

        if (link->network->arp < 0 &&
            link->network->multicast < 0 &&
            link->network->allmulticast < 0 &&
            link->network->promiscuous < 0)
                return 0;

        return link_request_set_link(link, SET_LINK_FLAGS, link_set_flags_handler, NULL);
}

int link_request_to_set_mtu(Link *link, uint32_t mtu) {
        Request *req = NULL;  /* avoid false maybe-uninitialized warning */
        int r;

        assert(link);

        /* IPv6 protocol requires a minimum MTU of IPV6_MTU_MIN(1280) bytes on the interface. Bump up
         * MTU bytes to IPV6_MTU_MIN. */
        if (mtu < IPV6_MIN_MTU && link_ipv6_enabled(link)) {
                log_link_warning(link, "Bumping MTU to " STRINGIFY(IPV6_MIN_MTU) ", as IPv6 is enabled "
                                 "and requires a minimum MTU of " STRINGIFY(IPV6_MIN_MTU) " bytes");
                mtu = IPV6_MIN_MTU;
        }

        if (link->mtu == mtu)
                return 0;

        r = link_request_set_link(link, SET_LINK_MTU, link_set_mtu_handler, &req);
        if (r < 0)
                return r;

        req->userdata = UINT32_TO_PTR(mtu);
        return 0;
}
