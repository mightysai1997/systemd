/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "netdev-util.h"
#include "networkd-address.h"
#include "networkd-link.h"
#include "string-table.h"

static const char * const netdev_local_address_type_table[_NETDEV_LOCAL_ADDRESS_TYPE_MAX] = {
        [NETDEV_LOCAL_ADDRESS_IPV4LL]  = "ipv4_link_local",
        [NETDEV_LOCAL_ADDRESS_IPV6LL]  = "ipv6_link_local",
        [NETDEV_LOCAL_ADDRESS_DHCP4]   = "dhcp4",
        [NETDEV_LOCAL_ADDRESS_DHCP6]   = "dhcp6",
        [NETDEV_LOCAL_ADDRESS_SLAAC]   = "slaac",
        [NETDEV_LOCAL_ADDRESS_AUTO]    = "auto",
        [NETDEV_LOCAL_ADDRESS_STATIC]  = "static",
        [NETDEV_LOCAL_ADDRESS_DYNAMIC] = "dynamic",
};

DEFINE_STRING_TABLE_LOOKUP(netdev_local_address_type, NetDevLocalAddressType);

int link_get_local_address(Link *link, NetDevLocalAddressType type, int family, union in_addr_union *ret) {
        Address *a;

        assert(link);

        switch (type) {
        case NETDEV_LOCAL_ADDRESS_IPV4LL:
                assert(family == AF_INET);
                break;
        case NETDEV_LOCAL_ADDRESS_IPV6LL:
                assert(family == AF_INET6);
                break;
        case NETDEV_LOCAL_ADDRESS_DHCP4:
                assert(family == AF_INET);
                break;
        case NETDEV_LOCAL_ADDRESS_DHCP6:
                assert(family == AF_INET6);
                break;
        case NETDEV_LOCAL_ADDRESS_SLAAC:
                assert(family == AF_INET6);
                break;
        case NETDEV_LOCAL_ADDRESS_AUTO:
        case NETDEV_LOCAL_ADDRESS_STATIC:
        case NETDEV_LOCAL_ADDRESS_DYNAMIC:
                assert(IN_SET(family, AF_INET, AF_INET6));
                break;
        default:
                assert_not_reached();
        }

        SET_FOREACH(a, link->addresses) {
                if (!address_exists(a))
                        continue;

                if (a->family != family)
                        continue;

                if (in_addr_is_set(a->family, &a->in_addr_peer))
                        continue;

                switch (type) {
                case NETDEV_LOCAL_ADDRESS_IPV4LL:
                        if (a->source != NETWORK_CONFIG_SOURCE_IPV4LL)
                                continue;
                        break;
                case NETDEV_LOCAL_ADDRESS_IPV6LL:
                        if (!in6_addr_is_link_local(&a->in_addr.in6))
                                continue;
                        break;
                case NETDEV_LOCAL_ADDRESS_DHCP4:
                        if (a->source != NETWORK_CONFIG_SOURCE_DHCP4)
                                continue;
                        break;
                case NETDEV_LOCAL_ADDRESS_DHCP6:
                        if (a->source != NETWORK_CONFIG_SOURCE_DHCP6)
                                continue;
                        break;
                case NETDEV_LOCAL_ADDRESS_SLAAC:
                        if (a->source != NETWORK_CONFIG_SOURCE_NDISC)
                                continue;
                        break;
                case NETDEV_LOCAL_ADDRESS_AUTO:
                        break;
                case NETDEV_LOCAL_ADDRESS_STATIC:
                        if (!FLAGS_SET(a->flags, IFA_F_PERMANENT))
                                continue;
                        break;
                case NETDEV_LOCAL_ADDRESS_DYNAMIC:
                        if (FLAGS_SET(a->flags, IFA_F_PERMANENT))
                                continue;
                        break;
                default:
                        assert_not_reached();
                }

                if (ret)
                        *ret = a->in_addr;
                return 1;
        }

        return -ENXIO;
}
