/* SPDX-License-Identifier: LGPL-2.1-or-later */
/***
  Copyright © 2015-2017 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
***/

#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_arp.h>
#include <linux/ipv6_route.h>

#include "sd-resolve.h"

#include "alloc-util.h"
#include "event-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "hexdecoct.h"
#include "memory-util.h"
#include "netlink-util.h"
#include "networkd-manager.h"
#include "networkd-route-util.h"
#include "networkd-route.h"
#include "networkd-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "resolve-private.h"
#include "string-util.h"
#include "strv.h"
#include "wireguard.h"

static void resolve_endpoints(NetDev *netdev);

static WireguardPeer* wireguard_peer_free(WireguardPeer *peer) {
        WireguardIPmask *mask;

        if (!peer)
                return NULL;

        if (peer->wireguard) {
                LIST_REMOVE(peers, peer->wireguard->peers, peer);

                set_remove(peer->wireguard->peers_with_unresolved_endpoint, peer);
                set_remove(peer->wireguard->peers_with_failed_endpoint, peer);

                if (peer->section)
                        hashmap_remove(peer->wireguard->peers_by_section, peer->section);
        }

        network_config_section_free(peer->section);

        while ((mask = peer->ipmasks)) {
                LIST_REMOVE(ipmasks, peer->ipmasks, mask);
                free(mask);
        }

        free(peer->endpoint_host);
        free(peer->endpoint_port);
        free(peer->preshared_key_file);
        explicit_bzero_safe(peer->preshared_key, WG_KEY_LEN);

        return mfree(peer);
}

DEFINE_NETWORK_SECTION_FUNCTIONS(WireguardPeer, wireguard_peer_free);

static int wireguard_peer_new_static(Wireguard *w, const char *filename, unsigned section_line, WireguardPeer **ret) {
        _cleanup_(network_config_section_freep) NetworkConfigSection *n = NULL;
        _cleanup_(wireguard_peer_freep) WireguardPeer *peer = NULL;
        int r;

        assert(w);
        assert(ret);
        assert(filename);
        assert(section_line > 0);

        r = network_config_section_new(filename, section_line, &n);
        if (r < 0)
                return r;

        peer = hashmap_get(w->peers_by_section, n);
        if (peer) {
                *ret = TAKE_PTR(peer);
                return 0;
        }

        peer = new(WireguardPeer, 1);
        if (!peer)
                return -ENOMEM;

        *peer = (WireguardPeer) {
                .flags = WGPEER_F_REPLACE_ALLOWEDIPS,
                .wireguard = w,
                .section = TAKE_PTR(n),
                .add_routes = -1,
        };

        LIST_PREPEND(peers, w->peers, peer);

        r = hashmap_ensure_put(&w->peers_by_section, &network_config_hash_ops, peer->section, peer);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(peer);
        return 0;
}

static int wireguard_set_ipmask_one(NetDev *netdev, sd_netlink_message *message, const WireguardIPmask *mask, uint16_t index) {
        int r;

        assert(message);
        assert(mask);
        assert(index > 0);

        /* This returns 1 on success, 0 on recoverable error, and negative errno on failure. */

        r = sd_netlink_message_open_array(message, index);
        if (r < 0)
                return 0;

        r = sd_netlink_message_append_u16(message, WGALLOWEDIP_A_FAMILY, mask->family);
        if (r < 0)
                goto cancel;

        r = netlink_message_append_in_addr_union(message, WGALLOWEDIP_A_IPADDR, mask->family, &mask->ip);
        if (r < 0)
                goto cancel;

        r = sd_netlink_message_append_u8(message, WGALLOWEDIP_A_CIDR_MASK, mask->cidr);
        if (r < 0)
                goto cancel;

        r = sd_netlink_message_close_container(message);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not add wireguard allowed ip: %m");

        return 1;

cancel:
        r = sd_netlink_message_cancel_array(message);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not cancel wireguard allowed ip message attribute: %m");

        return 0;
}

static int wireguard_set_peer_one(NetDev *netdev, sd_netlink_message *message, const WireguardPeer *peer, uint16_t index, WireguardIPmask **mask_start) {
        WireguardIPmask *mask, *start;
        uint16_t j = 0;
        int r;

        assert(message);
        assert(peer);
        assert(index > 0);
        assert(mask_start);

        /* This returns 1 on success, 0 on recoverable error, and negative errno on failure. */

        start = *mask_start ?: peer->ipmasks;

        r = sd_netlink_message_open_array(message, index);
        if (r < 0)
                return 0;

        r = sd_netlink_message_append_data(message, WGPEER_A_PUBLIC_KEY, &peer->public_key, sizeof(peer->public_key));
        if (r < 0)
                goto cancel;

        if (!*mask_start) {
                r = sd_netlink_message_append_data(message, WGPEER_A_PRESHARED_KEY, &peer->preshared_key, WG_KEY_LEN);
                if (r < 0)
                        goto cancel;

                r = sd_netlink_message_append_u32(message, WGPEER_A_FLAGS, peer->flags);
                if (r < 0)
                        goto cancel;

                r = sd_netlink_message_append_u16(message, WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL, peer->persistent_keepalive_interval);
                if (r < 0)
                        goto cancel;

                if (IN_SET(peer->endpoint.sa.sa_family, AF_INET, AF_INET6)) {
                        r = netlink_message_append_sockaddr_union(message, WGPEER_A_ENDPOINT, &peer->endpoint);
                        if (r < 0)
                                goto cancel;
                }
        }

        r = sd_netlink_message_open_container(message, WGPEER_A_ALLOWEDIPS);
        if (r < 0)
                goto cancel;

        LIST_FOREACH(ipmasks, mask, start) {
                r = wireguard_set_ipmask_one(netdev, message, mask, ++j);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;
        }

        r = sd_netlink_message_close_container(message);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not add wireguard allowed ip: %m");

        r = sd_netlink_message_close_container(message);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not add wireguard peer: %m");

        *mask_start = mask; /* Start next cycle from this mask. */
        return !mask;

cancel:
        r = sd_netlink_message_cancel_array(message);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not cancel wireguard peers: %m");

        return 0;
}

static int wireguard_set_interface(NetDev *netdev) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *message = NULL;
        WireguardIPmask *mask_start = NULL;
        WireguardPeer *peer, *peer_start;
        bool sent_once = false;
        uint32_t serial;
        Wireguard *w;
        int r;

        assert(netdev);
        w = WIREGUARD(netdev);
        assert(w);

        for (peer_start = w->peers; peer_start || !sent_once; ) {
                uint16_t i = 0;

                message = sd_netlink_message_unref(message);

                r = sd_genl_message_new(netdev->manager->genl, WG_GENL_NAME, WG_CMD_SET_DEVICE, &message);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Failed to allocate generic netlink message: %m");

                r = sd_netlink_message_append_string(message, WGDEVICE_A_IFNAME, netdev->ifname);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append wireguard interface name: %m");

                if (peer_start == w->peers) {
                        r = sd_netlink_message_append_data(message, WGDEVICE_A_PRIVATE_KEY, &w->private_key, WG_KEY_LEN);
                        if (r < 0)
                                return log_netdev_error_errno(netdev, r, "Could not append wireguard private key: %m");

                        r = sd_netlink_message_append_u16(message, WGDEVICE_A_LISTEN_PORT, w->port);
                        if (r < 0)
                                return log_netdev_error_errno(netdev, r, "Could not append wireguard port: %m");

                        r = sd_netlink_message_append_u32(message, WGDEVICE_A_FWMARK, w->fwmark);
                        if (r < 0)
                                return log_netdev_error_errno(netdev, r, "Could not append wireguard fwmark: %m");

                        r = sd_netlink_message_append_u32(message, WGDEVICE_A_FLAGS, w->flags);
                        if (r < 0)
                                return log_netdev_error_errno(netdev, r, "Could not append wireguard flags: %m");
                }

                r = sd_netlink_message_open_container(message, WGDEVICE_A_PEERS);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append wireguard peer attributes: %m");

                LIST_FOREACH(peers, peer, peer_start) {
                        r = wireguard_set_peer_one(netdev, message, peer, ++i, &mask_start);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                break;
                }
                peer_start = peer; /* Start next cycle from this peer. */

                r = sd_netlink_message_close_container(message);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not close wireguard container: %m");

                r = sd_netlink_send(netdev->manager->genl, message, &serial);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not set wireguard device: %m");

                sent_once = true;
        }

        return 0;
}

static void wireguard_peer_destroy_callback(WireguardPeer *peer) {
        NetDev *netdev;

        assert(peer);
        assert(peer->wireguard);

        netdev = NETDEV(peer->wireguard);

        if (section_is_invalid(peer->section))
                wireguard_peer_free(peer);

        netdev_unref(netdev);
}

static int on_resolve_retry(sd_event_source *s, usec_t usec, void *userdata) {
        NetDev *netdev = userdata;
        Wireguard *w;

        assert(netdev);
        w = WIREGUARD(netdev);
        assert(w);

        if (!netdev_is_managed(netdev))
                return 0;

        assert(set_isempty(w->peers_with_unresolved_endpoint));

        SWAP_TWO(w->peers_with_unresolved_endpoint, w->peers_with_failed_endpoint);

        resolve_endpoints(netdev);

        return 0;
}

/*
 * Given the number of retries this function will return will an exponential
 * increasing time in milliseconds to wait starting at 200ms and capped at 25 seconds.
 */
static int exponential_backoff_milliseconds(unsigned n_retries) {
        return (2 << MIN(n_retries, 7U)) * 100 * USEC_PER_MSEC;
}

static int wireguard_resolve_handler(sd_resolve_query *q,
                                     int ret,
                                     const struct addrinfo *ai,
                                     WireguardPeer *peer) {
        NetDev *netdev;
        Wireguard *w;
        int r;

        assert(peer);
        assert(peer->wireguard);

        w = peer->wireguard;
        netdev = NETDEV(w);

        if (!netdev_is_managed(netdev))
                return 0;

        if (ret != 0) {
                log_netdev_error(netdev, "Failed to resolve host '%s:%s': %s", peer->endpoint_host, peer->endpoint_port, gai_strerror(ret));

                r = set_ensure_put(&w->peers_with_failed_endpoint, NULL, peer);
                if (r < 0) {
                        log_netdev_error_errno(netdev, r, "Failed to save a peer, dropping the peer: %m");
                        peer->section->invalid = true;
                        goto resolve_next;
                }

        } else if ((ai->ai_family == AF_INET && ai->ai_addrlen == sizeof(struct sockaddr_in)) ||
                   (ai->ai_family == AF_INET6 && ai->ai_addrlen == sizeof(struct sockaddr_in6)))
                memcpy(&peer->endpoint, ai->ai_addr, ai->ai_addrlen);
        else
                log_netdev_error(netdev, "Neither IPv4 nor IPv6 address found for peer endpoint %s:%s, ignoring the address.",
                                 peer->endpoint_host, peer->endpoint_port);

resolve_next:
        if (!set_isempty(w->peers_with_unresolved_endpoint)) {
                resolve_endpoints(netdev);
                return 0;
        }

        (void) wireguard_set_interface(netdev);

        if (!set_isempty(w->peers_with_failed_endpoint)) {
                usec_t usec;

                w->n_retries++;
                usec = usec_add(now(CLOCK_MONOTONIC), exponential_backoff_milliseconds(w->n_retries));
                r = event_reset_time(netdev->manager->event, &w->resolve_retry_event_source,
                                     CLOCK_MONOTONIC, usec, 0, on_resolve_retry, netdev,
                                     0, "wireguard-resolve-retry", true);
                if (r < 0) {
                        log_netdev_warning_errno(netdev, r, "Could not arm resolve retry handler: %m");
                        return 0;
                }
        }

        return 0;
}

static void resolve_endpoints(NetDev *netdev) {
        static const struct addrinfo hints = {
                .ai_family = AF_UNSPEC,
                .ai_socktype = SOCK_DGRAM,
                .ai_protocol = IPPROTO_UDP
        };
        WireguardPeer *peer;
        Wireguard *w;
        int r;

        assert(netdev);
        w = WIREGUARD(netdev);
        assert(w);

        SET_FOREACH(peer, w->peers_with_unresolved_endpoint) {
                r = resolve_getaddrinfo(netdev->manager->resolve,
                                        NULL,
                                        peer->endpoint_host,
                                        peer->endpoint_port,
                                        &hints,
                                        wireguard_resolve_handler,
                                        wireguard_peer_destroy_callback,
                                        peer);
                if (r == -ENOBUFS)
                        break;
                if (r < 0) {
                        log_netdev_error_errno(netdev, r, "Failed to create resolver: %m");
                        continue;
                }

                /* Avoid freeing netdev. It will be unrefed by the destroy callback. */
                netdev_ref(netdev);

                (void) set_remove(w->peers_with_unresolved_endpoint, peer);
        }
}

static int netdev_wireguard_post_create(NetDev *netdev, Link *link, sd_netlink_message *m) {
        assert(netdev);
        assert(WIREGUARD(netdev));

        (void) wireguard_set_interface(netdev);
        resolve_endpoints(netdev);
        return 0;
}

int config_parse_wireguard_listen_port(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        uint16_t *s = data;
        int r;

        assert(rvalue);
        assert(data);

        if (isempty(rvalue) || streq(rvalue, "auto")) {
                *s = 0;
                return 0;
        }

        r = parse_ip_port(rvalue, s);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Invalid port specification, ignoring assignment: %s", rvalue);
                return 0;
        }

        return 0;
}

static int wireguard_decode_key_and_warn(
                const char *rvalue,
                uint8_t ret[static WG_KEY_LEN],
                const char *unit,
                const char *filename,
                unsigned line,
                const char *lvalue) {

        _cleanup_(erase_and_freep) void *key = NULL;
        size_t len;
        int r;

        assert(rvalue);
        assert(ret);
        assert(filename);
        assert(lvalue);

        if (isempty(rvalue)) {
                memzero(ret, WG_KEY_LEN);
                return 0;
        }

        if (!streq(lvalue, "PublicKey"))
                (void) warn_file_is_world_accessible(filename, NULL, unit, line);

        r = unbase64mem_full(rvalue, strlen(rvalue), true, &key, &len);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to decode wireguard key provided by %s=, ignoring assignment: %m", lvalue);
                return 0;
        }
        if (len != WG_KEY_LEN) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Wireguard key provided by %s= has invalid length (%zu bytes), ignoring assignment.",
                           lvalue, len);
                return 0;
        }

        memcpy(ret, key, WG_KEY_LEN);
        return 0;
}

int config_parse_wireguard_private_key(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Wireguard *w;

        assert(data);
        w = WIREGUARD(data);
        assert(w);

        return wireguard_decode_key_and_warn(rvalue, w->private_key, unit, filename, line, lvalue);
}

int config_parse_wireguard_private_key_file(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_free_ char *path = NULL;
        Wireguard *w;

        assert(data);
        w = WIREGUARD(data);
        assert(w);

        if (isempty(rvalue)) {
                w->private_key_file = mfree(w->private_key_file);
                return 0;
        }

        path = strdup(rvalue);
        if (!path)
                return log_oom();

        if (path_simplify_and_warn(path, PATH_CHECK_ABSOLUTE, unit, filename, line, lvalue) < 0)
                return 0;

        return free_and_replace(w->private_key_file, path);
}

int config_parse_wireguard_peer_key(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(wireguard_peer_free_or_set_invalidp) WireguardPeer *peer = NULL;
        Wireguard *w;
        int r;

        assert(data);
        w = WIREGUARD(data);
        assert(w);

        r = wireguard_peer_new_static(w, filename, section_line, &peer);
        if (r < 0)
                return log_oom();

        r = wireguard_decode_key_and_warn(rvalue,
                                          streq(lvalue, "PublicKey") ? peer->public_key : peer->preshared_key,
                                          unit, filename, line, lvalue);
        if (r < 0)
                return r;

        TAKE_PTR(peer);
        return 0;
}

int config_parse_wireguard_preshared_key_file(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(wireguard_peer_free_or_set_invalidp) WireguardPeer *peer = NULL;
        _cleanup_free_ char *path = NULL;
        Wireguard *w;
        int r;

        assert(data);
        w = WIREGUARD(data);
        assert(w);

        r = wireguard_peer_new_static(w, filename, section_line, &peer);
        if (r < 0)
                return log_oom();

        if (isempty(rvalue)) {
                peer->preshared_key_file = mfree(peer->preshared_key_file);
                TAKE_PTR(peer);
                return 0;
        }

        path = strdup(rvalue);
        if (!path)
                return log_oom();

        if (path_simplify_and_warn(path, PATH_CHECK_ABSOLUTE, unit, filename, line, lvalue) < 0)
                return 0;

        free_and_replace(peer->preshared_key_file, path);
        TAKE_PTR(peer);
        return 0;
}

int config_parse_wireguard_allowed_ips(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(wireguard_peer_free_or_set_invalidp) WireguardPeer *peer = NULL;
        union in_addr_union addr;
        unsigned char prefixlen;
        int r, family;
        Wireguard *w;
        WireguardIPmask *ipmask;

        assert(rvalue);
        assert(data);

        w = WIREGUARD(data);
        assert(w);

        r = wireguard_peer_new_static(w, filename, section_line, &peer);
        if (r < 0)
                return log_oom();

        for (const char *p = rvalue;;) {
                _cleanup_free_ char *word = NULL;

                r = extract_first_word(&p, &word, "," WHITESPACE, 0);
                if (r == 0)
                        break;
                if (r == -ENOMEM)
                        return log_oom();
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Failed to split allowed ips \"%s\" option: %m", rvalue);
                        break;
                }

                r = in_addr_prefix_from_string_auto(word, &family, &addr, &prefixlen);
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Network address is invalid, ignoring assignment: %s", word);
                        continue;
                }

                ipmask = new(WireguardIPmask, 1);
                if (!ipmask)
                        return log_oom();

                *ipmask = (WireguardIPmask) {
                        .family = family,
                        .ip = addr,
                        .cidr = prefixlen,
                };

                LIST_PREPEND(ipmasks, peer->ipmasks, ipmask);
        }

        TAKE_PTR(peer);
        return 0;
}

int config_parse_wireguard_endpoint(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(wireguard_peer_free_or_set_invalidp) WireguardPeer *peer = NULL;
        const char *begin, *end;
        Wireguard *w;
        size_t len;
        int r;

        assert(data);
        assert(rvalue);

        w = WIREGUARD(data);
        assert(w);

        if (rvalue[0] == '[') {
                begin = &rvalue[1];
                end = strchr(rvalue, ']');
                if (!end) {
                        log_syntax(unit, LOG_WARNING, filename, line, 0,
                                   "Unable to find matching brace of endpoint, ignoring assignment: %s",
                                   rvalue);
                        return 0;
                }
                len = end - begin;
                ++end;
                if (*end != ':' || !*(end + 1)) {
                        log_syntax(unit, LOG_WARNING, filename, line, 0,
                                   "Unable to find port of endpoint, ignoring assignment: %s",
                                   rvalue);
                        return 0;
                }
                ++end;
        } else {
                begin = rvalue;
                end = strrchr(rvalue, ':');
                if (!end || !*(end + 1)) {
                        log_syntax(unit, LOG_WARNING, filename, line, 0,
                                   "Unable to find port of endpoint, ignoring assignment: %s",
                                   rvalue);
                        return 0;
                }
                len = end - begin;
                ++end;
        }

        r = wireguard_peer_new_static(w, filename, section_line, &peer);
        if (r < 0)
                return log_oom();

        r = free_and_strndup(&peer->endpoint_host, begin, len);
        if (r < 0)
                return log_oom();

        r = free_and_strdup(&peer->endpoint_port, end);
        if (r < 0)
                return log_oom();

        r = set_ensure_put(&w->peers_with_unresolved_endpoint, NULL, peer);
        if (r < 0)
                return log_oom();
        TAKE_PTR(peer); /* The peer may already have been in the hash map, that is fine too. */

        return 0;
}

int config_parse_wireguard_keepalive(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(wireguard_peer_free_or_set_invalidp) WireguardPeer *peer = NULL;
        uint16_t keepalive = 0;
        Wireguard *w;
        int r;

        assert(rvalue);
        assert(data);

        w = WIREGUARD(data);
        assert(w);

        r = wireguard_peer_new_static(w, filename, section_line, &peer);
        if (r < 0)
                return log_oom();

        if (streq(rvalue, "off"))
                keepalive = 0;
        else {
                r = safe_atou16(rvalue, &keepalive);
                if (r < 0) {
                        log_syntax(unit, LOG_WARNING, filename, line, r,
                                   "Failed to parse \"%s\" as keepalive interval (range 0–65535), ignoring assignment: %m",
                                   rvalue);
                        return 0;
                }
        }

        peer->persistent_keepalive_interval = keepalive;

        TAKE_PTR(peer);
        return 0;
}

int config_parse_wireguard_add_routes(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        bool *add_routes = data;
        int r;

        assert(filename);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                *add_routes = false;
                return 0;
        }

        r = parse_boolean(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse %s=, ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        *add_routes = r;
        return 0;
}

int config_parse_wireguard_peer_add_routes(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(wireguard_peer_free_or_set_invalidp) WireguardPeer *peer = NULL;
        Wireguard *w;
        int r;

        assert(filename);
        assert(rvalue);
        assert(userdata);

        w = WIREGUARD(userdata);
        assert(w);

        r = wireguard_peer_new_static(w, filename, section_line, &peer);
        if (r < 0)
                return log_oom();

        if (isempty(rvalue)) {
                peer->add_routes = -1;
                TAKE_PTR(peer);
                return 0;
        }

        r = parse_boolean(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse %s=, ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        peer->add_routes = r;
        TAKE_PTR(peer);
        return 0;
}

int config_parse_wireguard_route_table(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        NetDev *netdev = userdata;
        uint32_t *table = data;
        int r;

        assert(filename);
        assert(rvalue);
        assert(data);
        assert(userdata);

        if (isempty(rvalue)) {
                *table = RT_TABLE_MAIN;
                return 0;
        }

        r = manager_get_route_table_from_string(netdev->manager, rvalue, table);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse %s=, ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        return 0;
}

int config_parse_wireguard_peer_route_table(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(wireguard_peer_free_or_set_invalidp) WireguardPeer *peer = NULL;
        NetDev *netdev = userdata;
        Wireguard *w;
        int r;

        assert(filename);
        assert(rvalue);
        assert(netdev);
        assert(netdev->manager);

        w = WIREGUARD(netdev);
        assert(w);

        r = wireguard_peer_new_static(w, filename, section_line, &peer);
        if (r < 0)
                return log_oom();

        if (isempty(rvalue)) {
                peer->route_table_set = false;
                TAKE_PTR(peer);
                return 0;
        }

        r = manager_get_route_table_from_string(netdev->manager, rvalue, &peer->route_table);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse %s=, ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        peer->route_table_set = true;
        TAKE_PTR(peer);
        return 0;
}

int config_parse_wireguard_route_priority(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        uint32_t *priority = data;
        int r;

        assert(filename);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                *priority = 0;
                return 0;
        }

        r = safe_atou32(rvalue, priority);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse route priority \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        return 0;
}

int config_parse_wireguard_peer_route_priority(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(wireguard_peer_free_or_set_invalidp) WireguardPeer *peer = NULL;
        Wireguard *w;
        int r;

        assert(filename);
        assert(rvalue);
        assert(userdata);

        w = WIREGUARD(userdata);
        assert(w);

        r = wireguard_peer_new_static(w, filename, section_line, &peer);
        if (r < 0)
                return log_oom();

        if (isempty(rvalue)) {
                peer->route_priority_set = false;
                TAKE_PTR(peer);
                return 0;
        }

        r = safe_atou32(rvalue, &peer->route_priority);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse route priority \"%s\", ignoring assignment: %m", rvalue);
                return 0;
        }

        peer->route_priority_set = true;
        TAKE_PTR(peer);
        return 0;
}

static void wireguard_init(NetDev *netdev) {
        Wireguard *w;

        assert(netdev);
        w = WIREGUARD(netdev);
        assert(w);

        w->flags = WGDEVICE_F_REPLACE_PEERS;
        w->route_table = RT_TABLE_MAIN;
}

static void wireguard_done(NetDev *netdev) {
        Wireguard *w;

        assert(netdev);
        w = WIREGUARD(netdev);
        assert(w);

        sd_event_source_disable_unref(w->resolve_retry_event_source);

        explicit_bzero_safe(w->private_key, WG_KEY_LEN);
        free(w->private_key_file);

        hashmap_free_with_destructor(w->peers_by_section, wireguard_peer_free);
        set_free(w->peers_with_unresolved_endpoint);
        set_free(w->peers_with_failed_endpoint);

        set_free(w->routes);
}

static int wireguard_read_key_file(const char *filename, uint8_t dest[static WG_KEY_LEN]) {
        _cleanup_(erase_and_freep) char *key = NULL;
        size_t key_len;
        int r;

        if (!filename)
                return 0;

        assert(dest);

        (void) warn_file_is_world_accessible(filename, NULL, NULL, 0);

        r = read_full_file_full(
                        AT_FDCWD, filename, UINT64_MAX, SIZE_MAX,
                        READ_FULL_FILE_SECURE | READ_FULL_FILE_UNBASE64 | READ_FULL_FILE_WARN_WORLD_READABLE | READ_FULL_FILE_CONNECT_SOCKET,
                        NULL, &key, &key_len);
        if (r < 0)
                return r;

        if (key_len != WG_KEY_LEN)
                return -EINVAL;

        memcpy(dest, key, WG_KEY_LEN);
        return 0;
}

static int wireguard_peer_verify(WireguardPeer *peer) {
        NetDev *netdev = NETDEV(peer->wireguard);
        int r;

        if (section_is_invalid(peer->section))
                return -EINVAL;

        if (eqzero(peer->public_key))
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                              "%s: WireGuardPeer section without PublicKey= configured. "
                                              "Ignoring [WireGuardPeer] section from line %u.",
                                              peer->section->filename, peer->section->line);

        r = wireguard_read_key_file(peer->preshared_key_file, peer->preshared_key);
        if (r < 0)
                return log_netdev_error_errno(netdev, r,
                                              "%s: Failed to read preshared key from '%s'. "
                                              "Ignoring [WireGuardPeer] section from line %u.",
                                              peer->section->filename, peer->preshared_key_file,
                                              peer->section->line);

        return 0;
}

static int wireguard_verify(NetDev *netdev, const char *filename) {
        WireguardPeer *peer, *peer_next;
        Wireguard *w;
        int r;

        assert(netdev);
        w = WIREGUARD(netdev);
        assert(w);

        r = wireguard_read_key_file(w->private_key_file, w->private_key);
        if (r < 0)
                return log_netdev_error_errno(netdev, r,
                                              "Failed to read private key from %s. Ignoring network device.",
                                              w->private_key_file);

        if (eqzero(w->private_key))
                return log_netdev_error_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                              "%s: Missing PrivateKey= or PrivateKeyFile=, "
                                              "Ignoring network device.", filename);

        LIST_FOREACH_SAFE(peers, peer, peer_next, w->peers) {
                WireguardIPmask *ipmask;

                if (wireguard_peer_verify(peer) < 0) {
                        wireguard_peer_free(peer);
                        continue;
                }

                if ((peer->add_routes < 0 && !w->add_routes) || !peer->add_routes)
                        continue;

                LIST_FOREACH(ipmasks, ipmask, peer->ipmasks) {
                        _cleanup_(route_freep) Route *route = NULL;

                        r = route_new(&route);
                        if (r < 0)
                                return log_oom();

                        route->family = ipmask->family;
                        route->dst = ipmask->ip;
                        route->dst_prefixlen = ipmask->cidr;
                        route->scope = RT_SCOPE_UNIVERSE;
                        route->protocol = RTPROT_STATIC;
                        route->table = peer->route_table_set ? peer->route_table : w->route_table;
                        route->priority = peer->route_priority_set ? peer->route_priority : w->route_priority;
                        if (route->priority == 0 && route->family == AF_INET6)
                                route->priority = IP6_RT_PRIO_USER;
                        route->source = NETWORK_CONFIG_SOURCE_STATIC;

                        r = set_ensure_consume(&w->routes, &route_hash_ops, TAKE_PTR(route));
                        if (r < 0)
                                return r;
                }
        }

        return 0;
}

const NetDevVTable wireguard_vtable = {
        .object_size = sizeof(Wireguard),
        .sections = NETDEV_COMMON_SECTIONS "WireGuard\0WireGuardPeer\0",
        .post_create = netdev_wireguard_post_create,
        .init = wireguard_init,
        .done = wireguard_done,
        .create_type = NETDEV_CREATE_INDEPENDENT,
        .config_verify = wireguard_verify,
        .iftype = ARPHRD_NONE,
};
