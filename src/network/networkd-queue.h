/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "sd-event.h"
#include "sd-netlink.h"

#include "alloc-util.h"
#include "hash-funcs.h"

typedef struct Link Link;
typedef struct NetDev NetDev;
typedef struct Manager Manager;
typedef struct Request Request;

typedef int (*request_is_ready_func_t)(Request *req);
typedef int (*request_process_func_t)(Request *req);
typedef int (*request_netlink_handler_t)(sd_netlink *nl, sd_netlink_message *m, Request *req);

typedef enum RequestType {
        REQUEST_TYPE_ACTIVATE_LINK,
        REQUEST_TYPE_ADDRESS,
        REQUEST_TYPE_ADDRESS_LABEL,
        REQUEST_TYPE_BRIDGE_FDB,
        REQUEST_TYPE_BRIDGE_MDB,
        REQUEST_TYPE_DHCP_SERVER,
        REQUEST_TYPE_DHCP4_CLIENT,
        REQUEST_TYPE_DHCP6_CLIENT,
        REQUEST_TYPE_IPV6_PROXY_NDP,
        REQUEST_TYPE_NDISC,
        REQUEST_TYPE_NEIGHBOR,
        REQUEST_TYPE_NETDEV_CONFIGURE,
        REQUEST_TYPE_NETDEV_INDEPENDENT,
        REQUEST_TYPE_NETDEV_STACKED,
        REQUEST_TYPE_NEXTHOP,
        REQUEST_TYPE_RADV,
        REQUEST_TYPE_ROUTE,
        REQUEST_TYPE_ROUTING_POLICY_RULE,
        REQUEST_TYPE_SET_LINK_ADDRESS_GENERATION_MODE, /* Setting IPv6LL address generation mode. */
        REQUEST_TYPE_SET_LINK_BOND,                    /* Setting bond configs. */
        REQUEST_TYPE_SET_LINK_BRIDGE,                  /* Setting bridge configs. */
        REQUEST_TYPE_SET_LINK_BRIDGE_VLAN,             /* Setting bridge VLAN configs. */
        REQUEST_TYPE_SET_LINK_CAN,                     /* Setting CAN interface configs. */
        REQUEST_TYPE_SET_LINK_FLAGS,                   /* Setting IFF_NOARP or friends. */
        REQUEST_TYPE_SET_LINK_GROUP,                   /* Setting interface group. */
        REQUEST_TYPE_SET_LINK_IPOIB,                   /* Setting IPoIB configs. */
        REQUEST_TYPE_SET_LINK_MAC,                     /* Setting MAC address. */
        REQUEST_TYPE_SET_LINK_MASTER,                  /* Setting IFLA_MASTER. */
        REQUEST_TYPE_SET_LINK_MTU,                     /* Setting MTU. */
        REQUEST_TYPE_TC_QDISC,
        REQUEST_TYPE_TC_CLASS,
        REQUEST_TYPE_UP_DOWN,
        _REQUEST_TYPE_MAX,
        _REQUEST_TYPE_INVALID = -EINVAL,
} RequestType;

struct Request {
        unsigned n_ref;

        Manager *manager; /* must be non-NULL */
        Link *link; /* can be NULL */

        RequestType type;

        /* Target object, e.g. Address, Route, NetDev, and so on. */
        void *userdata;
        /* freeing userdata when the request is completed or failed. */
        mfree_func_t free_func;

        /* hash and compare functions for userdata, used for dedup requests. */
        hash_func_t hash_func;
        compare_func_t compare_func;

        request_is_ready_func_t is_ready; /* return true when this request is ready to process. */
        request_process_func_t process; /* process this request, e.g. call address_configure() */

        /* incremented when requested, decremented when request is completed or failed. */
        unsigned *counter;
        /* called in netlink handler, the 'counter' is decremented before this is called.
         * If this is specified, then the 'process' function must increment the reference of this
         * request, and pass this request to the netlink_call_async(), and set the destroy function
         * to the slot. */
        request_netlink_handler_t netlink_handler;
};

Request *request_ref(Request *req);
Request *request_unref(Request *req);
DEFINE_TRIVIAL_CLEANUP_FUNC(Request*, request_unref);

int netdev_queue_request(
                NetDev *netdev,
                request_is_ready_func_t is_ready,
                request_process_func_t process,
                Request **ret);

int link_queue_request_full(
                Link *link,
                RequestType type,
                void *userdata,
                mfree_func_t free_func,
                hash_func_t hash_func,
                compare_func_t compare_func,
                request_is_ready_func_t is_ready,
                request_process_func_t process,
                unsigned *counter,
                request_netlink_handler_t netlink_handler,
                Request **ret);

static inline int link_queue_request(
                Link *link,
                RequestType type,
                request_is_ready_func_t is_ready,
                request_process_func_t process,
                Request **ret) {

        return link_queue_request_full(link, type, NULL, NULL, NULL, NULL,
                                       is_ready, process, NULL, NULL, ret);
}

int manager_process_requests(sd_event_source *s, void *userdata);
int request_call_netlink_async(sd_netlink *nl, sd_netlink_message *m, Request *req);

const char* request_type_to_string(RequestType t) _const_;
