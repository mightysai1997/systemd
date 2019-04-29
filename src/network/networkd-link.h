/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include <endian.h>

#include "sd-bus.h"
#include "sd-device.h"
#include "sd-dhcp-client.h"
#include "sd-dhcp-server.h"
#include "sd-dhcp6-client.h"
#include "sd-ipv4ll.h"
#include "sd-lldp.h"
#include "sd-ndisc.h"
#include "sd-radv.h"
#include "sd-netlink.h"

#include "list.h"
#include "log-link.h"
#include "network-util.h"
#include "set.h"

typedef enum LinkState {
        LINK_STATE_PENDING,     /* udev has not initialized the link */
        LINK_STATE_INITIALIZED, /* udev has initialized the link */
        LINK_STATE_CONFIGURING, /* configuring addresses, routes, etc. */
        LINK_STATE_CONFIGURED,  /* everything is configured */
        LINK_STATE_UNMANAGED,   /* Unmanaged=yes is set */
        LINK_STATE_FAILED,      /* at least one configuration process failed */
        LINK_STATE_LINGER,      /* RTM_DELLINK for the link has been received */
        _LINK_STATE_MAX,
        _LINK_STATE_INVALID = -1
} LinkState;

typedef struct Manager Manager;
typedef struct Network Network;
typedef struct Address Address;
typedef struct DUID DUID;

typedef struct Link {
        Manager *manager;

        unsigned n_ref;

        int ifindex;
        int master_ifindex;
        char *ifname;
        char *kind;
        unsigned short iftype;
        char *state_file;
        struct ether_addr mac;
        struct in6_addr ipv6ll_address;
        uint32_t mtu;
        sd_device *sd_device;

        unsigned flags;
        uint8_t kernel_operstate;

        Network *network;

        LinkState state;
        LinkOperationalState operstate;

        unsigned address_messages;
        unsigned address_label_messages;
        unsigned neighbor_messages;
        unsigned route_messages;
        unsigned routing_policy_rule_messages;
        unsigned routing_policy_rule_remove_messages;
        unsigned enslaving;
        /* link_is_enslaved() has additional checks. So, it is named _raw. */
        bool enslaved_raw;

        Set *addresses;
        Set *addresses_foreign;
        Set *routes;
        Set *routes_foreign;

        bool addresses_configured;
        bool addresses_ready;

        sd_dhcp_client *dhcp_client;
        sd_dhcp_lease *dhcp_lease;
        char *lease_file;
        uint32_t original_mtu;
        unsigned dhcp4_messages;
        bool dhcp4_configured;
        bool dhcp6_configured;

        unsigned ndisc_messages;
        bool ndisc_configured;

        sd_ipv4ll *ipv4ll;
        bool ipv4ll_address:1;
        bool ipv4ll_route:1;

        bool neighbors_configured;

        bool static_routes_configured;
        bool routing_policy_rules_configured;
        bool setting_mtu;

        LIST_HEAD(Address, pool_addresses);

        sd_dhcp_server *dhcp_server;

        sd_ndisc *ndisc;
        Set *ndisc_rdnss;
        Set *ndisc_dnssl;

        sd_radv *radv;

        sd_dhcp6_client *dhcp6_client;
        bool rtnl_extended_attrs;

        /* This is about LLDP reception */
        sd_lldp *lldp;
        char *lldp_file;

        /* This is about LLDP transmission */
        unsigned lldp_tx_fast; /* The LLDP txFast counter (See 802.1ab-2009, section 9.2.5.18) */
        sd_event_source *lldp_emit_event_source;

        Hashmap *bound_by_links;
        Hashmap *bound_to_links;
        Set *slaves;
} Link;

typedef int (*link_netlink_message_handler_t)(sd_netlink*, sd_netlink_message*, Link*);

DUID *link_get_duid(Link *link);
int get_product_uuid_handler(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);

Link *link_unref(Link *link);
Link *link_ref(Link *link);
DEFINE_TRIVIAL_DESTRUCTOR(link_netlink_destroy_callback, Link, link_unref);

int link_get(Manager *m, int ifindex, Link **ret);
int link_add(Manager *manager, sd_netlink_message *message, Link **ret);
void link_drop(Link *link);

int link_down(Link *link);

void link_enter_failed(Link *link);
int link_initialized(Link *link, sd_device *device);

void link_check_ready(Link *link);

void link_update_operstate(Link *link, bool also_update_bond_master);
int link_update(Link *link, sd_netlink_message *message);

void link_dirty(Link *link);
void link_clean(Link *link);
int link_save(Link *link);

int link_carrier_reset(Link *link);
bool link_has_carrier(Link *link);

int link_ipv6ll_gained(Link *link, const struct in6_addr *address);

int link_set_mtu(Link *link, uint32_t mtu, bool force);

int ipv4ll_configure(Link *link);
int dhcp4_configure(Link *link);
int dhcp4_set_client_identifier(Link *link);
int dhcp4_set_promote_secondaries(Link *link);
int dhcp6_request_prefix_delegation(Link *link);
int dhcp6_configure(Link *link);
int dhcp6_request_address(Link *link, int ir);
int dhcp6_lease_pd_prefix_lost(sd_dhcp6_client *client, Link* link);

const char* link_state_to_string(LinkState s) _const_;
LinkState link_state_from_string(const char *s) _pure_;

extern const sd_bus_vtable link_vtable[];

int link_node_enumerator(sd_bus *bus, const char *path, void *userdata, char ***nodes, sd_bus_error *error);
int link_object_find(sd_bus *bus, const char *path, const char *interface, void *userdata, void **found, sd_bus_error *error);
int link_send_changed(Link *link, const char *property, ...) _sentinel_;

uint32_t link_get_vrf_table(Link *link);
uint32_t link_get_dhcp_route_table(Link *link);
uint32_t link_get_ipv6_accept_ra_route_table(Link *link);
int link_request_set_routes(Link *link);

#define ADDRESS_FMT_VAL(address)                   \
        be32toh((address).s_addr) >> 24,           \
        (be32toh((address).s_addr) >> 16) & 0xFFu, \
        (be32toh((address).s_addr) >> 8) & 0xFFu,  \
        be32toh((address).s_addr) & 0xFFu
