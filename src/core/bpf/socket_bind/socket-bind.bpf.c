/* SPDX-License-Identifier: LGPL-2.1-or-later */

/* The SPDX header above is actually correct in claiming this was
 * LGPL-2.1-or-later, because it is. Since the kernel doesn't consider that
 * compatible with GPL we will claim this to be GPL however, which should be
 * fine given that LGPL-2.1-or-later downgrades to GPL if needed.
 */

#include "socket-bind-api.bpf.h"
/* <linux/types.h> must precede <bpf/bpf_helpers.h> due to
 * <bpf/bpf_helpers.h> does not depend from type header by design.
 */
#include <linux/types.h>
#include <bpf/bpf_endian.h>
#include <bpf/bpf_helpers.h>
#include <linux/bpf.h>
#include <netinet/in.h>
#include <stdbool.h>

/*
 * max_entries is set from user space with bpf_map__resize helper.
 */
struct {
        __uint(type, BPF_MAP_TYPE_ARRAY);
        __type(key, __u32);
        __type(value, struct socket_bind_rule);
} rules SEC(".maps");

static __always_inline bool match_af(
                __u8 address_family, const struct socket_bind_rule *r) {
        return r->address_family == AF_UNSPEC || address_family == r->address_family;
}

static __always_inline bool match_user_port(
                __u16 port, const struct socket_bind_rule *r) {
        return r->nr_ports == 0 ||
                (port >= r->port_min && port < r->port_min + (__u32) r->nr_ports);
}

static __always_inline bool match(
                __u8 address_family,
                __u16 port,
                const struct socket_bind_rule *r) {
        return match_af(address_family, r) && match_user_port(port, r);
}

static __always_inline int socket_bind_impl(
                struct bpf_sock_addr* ctx) {
        volatile __u32 user_port = ctx->user_port;
        __u16 port = (__u16)bpf_ntohs(user_port);

        for (__u32 i = 0; i < socket_bind_max_rules; ++i) {
                const __u32 key = i;
                const struct socket_bind_rule *rule = bpf_map_lookup_elem(&rules, &key);

                /* Lookup returns NULL if iterator is advanced past the last
                 * element put in the map. */
                if (!rule)
                        break;

                if (match(ctx->user_family, port, rule))
                        return rule->action;
        }

        return SOCKET_BIND_ALLOW;
}

SEC("cgroup/bind6")
int socket_bind_v6(struct bpf_sock_addr* ctx) {
        if (ctx->user_family != AF_INET6 || ctx->family != AF_INET6)
                return SOCKET_BIND_ALLOW;

        return socket_bind_impl(ctx);
}

SEC("cgroup/bind4")
int socket_bind_v4(struct bpf_sock_addr* ctx) {
        if (ctx->user_family != AF_INET || ctx->family != AF_INET)
                return SOCKET_BIND_ALLOW;

        return socket_bind_impl(ctx);
}

char _license[] SEC("license") = "GPL";
