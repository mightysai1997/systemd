/* SPDX-License-Identifier: LGPL-2.1-or-later */
/***
  Copyright © 2017 Intel Corporation. All rights reserved.
***/

#include <netinet/icmp6.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sd-radv.h"

#include "alloc-util.h"
#include "dns-domain.h"
#include "ether-addr-util.h"
#include "event-util.h"
#include "fd-util.h"
#include "icmp6-util.h"
#include "in-addr-util.h"
#include "iovec-util.h"
#include "macro.h"
#include "memory-util.h"
#include "network-common.h"
#include "radv-internal.h"
#include "random-util.h"
#include "socket-util.h"
#include "string-util.h"
#include "strv.h"
#include "unaligned.h"

int sd_radv_new(sd_radv **ret) {
        _cleanup_(sd_radv_unrefp) sd_radv *ra = NULL;

        assert_return(ret, -EINVAL);

        ra = new(sd_radv, 1);
        if (!ra)
                return -ENOMEM;

        *ra = (sd_radv) {
                .n_ref = 1,
                .fd = -EBADF,
                .lifetime_usec = RADV_DEFAULT_ROUTER_LIFETIME_USEC,
        };

        *ret = TAKE_PTR(ra);

        return 0;
}

int sd_radv_attach_event(sd_radv *ra, sd_event *event, int64_t priority) {
        int r;

        assert_return(ra, -EINVAL);
        assert_return(!ra->event, -EBUSY);

        if (event)
                ra->event = sd_event_ref(event);
        else {
                r = sd_event_default(&ra->event);
                if (r < 0)
                        return 0;
        }

        ra->event_priority = priority;

        return 0;
}

int sd_radv_detach_event(sd_radv *ra) {

        assert_return(ra, -EINVAL);

        ra->event = sd_event_unref(ra->event);
        return 0;
}

sd_event *sd_radv_get_event(sd_radv *ra) {
        assert_return(ra, NULL);

        return ra->event;
}

int sd_radv_is_running(sd_radv *ra) {
        assert_return(ra, false);

        return ra->state != RADV_STATE_IDLE;
}

static void radv_reset(sd_radv *ra) {
        assert(ra);

        (void) event_source_disable(ra->timeout_event_source);

        ra->recv_event_source = sd_event_source_disable_unref(ra->recv_event_source);

        ra->ra_sent = 0;
}

static sd_radv *radv_free(sd_radv *ra) {
        if (!ra)
                return NULL;

        LIST_CLEAR(prefix, ra->prefixes, sd_radv_prefix_unref);
        LIST_CLEAR(prefix, ra->route_prefixes, sd_radv_route_prefix_unref);
        LIST_CLEAR(prefix, ra->pref64_prefixes, sd_radv_pref64_prefix_unref);

        free(ra->rdnss);
        free(ra->dnssl);

        radv_reset(ra);

        sd_event_source_unref(ra->timeout_event_source);
        sd_radv_detach_event(ra);

        ra->fd = safe_close(ra->fd);
        free(ra->ifname);

        return mfree(ra);
}

DEFINE_PUBLIC_TRIVIAL_REF_UNREF_FUNC(sd_radv, sd_radv, radv_free);

static bool router_lifetime_is_valid(usec_t lifetime_usec) {
        assert_cc(RADV_MAX_ROUTER_LIFETIME_USEC <= UINT16_MAX * USEC_PER_SEC);
        return lifetime_usec == 0 ||
                (lifetime_usec >= RADV_MIN_ROUTER_LIFETIME_USEC &&
                 lifetime_usec <= RADV_MAX_ROUTER_LIFETIME_USEC);
}

static int radv_send_router(sd_radv *ra, const struct in6_addr *dst, usec_t lifetime_usec) {
        assert(ra);
        assert(router_lifetime_is_valid(lifetime_usec));

        struct sockaddr_in6 dst_addr = {
                .sin6_family = AF_INET6,
                .sin6_addr = IN6ADDR_ALL_NODES_MULTICAST_INIT,
        };
        struct nd_router_advert adv = {
                .nd_ra_type = ND_ROUTER_ADVERT,
                .nd_ra_router_lifetime = usec_to_be16_sec(lifetime_usec),
                .nd_ra_retransmit = usec_to_be32_msec(ra->retransmit_usec),
        };
        struct {
                struct nd_opt_hdr opthdr;
                struct ether_addr slladdr;
        } _packed_ opt_mac = {
                .opthdr = {
                        .nd_opt_type = ND_OPT_SOURCE_LINKADDR,
                        .nd_opt_len = DIV_ROUND_UP(sizeof(struct nd_opt_hdr) + sizeof(struct ether_addr), 8),
                },
                .slladdr = ra->mac_addr,
        };
        struct nd_opt_mtu opt_mtu =  {
                .nd_opt_mtu_type = ND_OPT_MTU,
                .nd_opt_mtu_len = 1,
                .nd_opt_mtu_mtu = htobe32(ra->mtu),
        };
        /* Reserve iov space for RA header, linkaddr, MTU, N prefixes, N routes, N pref64 prefixes, RDNSS,
         * DNSSL, and home agent. */
        struct iovec iov[6 + ra->n_prefixes + ra->n_route_prefixes + ra->n_pref64_prefixes];
        struct msghdr msg = {
                .msg_name = &dst_addr,
                .msg_namelen = sizeof(dst_addr),
                .msg_iov = iov,
        };
        usec_t time_now;
        int r;

        r = sd_event_now(ra->event, CLOCK_BOOTTIME, &time_now);
        if (r < 0)
                return r;

        if (dst && in6_addr_is_set(dst))
                dst_addr.sin6_addr = *dst;

        /* The nd_ra_curhoplimit and nd_ra_flags_reserved fields cannot specified with nd_ra_router_lifetime
         * simultaneously in the structured initializer in the above. */
        adv.nd_ra_curhoplimit = ra->hop_limit;
        adv.nd_ra_flags_reserved = ra->flags;
        iov[msg.msg_iovlen++] = IOVEC_MAKE(&adv, sizeof(adv));

        /* MAC address is optional, either because the link does not use L2 addresses or load sharing is
         * desired. See RFC 4861, Section 4.2. */
        if (!ether_addr_is_null(&ra->mac_addr))
                iov[msg.msg_iovlen++] = IOVEC_MAKE(&opt_mac, sizeof(opt_mac));

        if (ra->mtu > 0)
                iov[msg.msg_iovlen++] = IOVEC_MAKE(&opt_mtu, sizeof(opt_mtu));

        LIST_FOREACH(prefix, p, ra->prefixes) {
                usec_t lifetime_valid_usec, lifetime_preferred_usec;

                lifetime_valid_usec = MIN(usec_sub_unsigned(p->valid_until, time_now),
                                          p->lifetime_valid_usec);

                lifetime_preferred_usec = MIN3(usec_sub_unsigned(p->preferred_until, time_now),
                                               p->lifetime_preferred_usec,
                                               lifetime_valid_usec);

                p->opt.lifetime_valid = usec_to_be32_sec(lifetime_valid_usec);
                p->opt.lifetime_preferred = usec_to_be32_sec(lifetime_preferred_usec);

                iov[msg.msg_iovlen++] = IOVEC_MAKE(&p->opt, sizeof(p->opt));
        }

        LIST_FOREACH(prefix, rt, ra->route_prefixes) {
                rt->opt.lifetime = usec_to_be32_sec(MIN(usec_sub_unsigned(rt->valid_until, time_now),
                                                        rt->lifetime_usec));

                iov[msg.msg_iovlen++] = IOVEC_MAKE(&rt->opt, sizeof(rt->opt));
        }

        LIST_FOREACH(prefix, p, ra->pref64_prefixes)
                iov[msg.msg_iovlen++] = IOVEC_MAKE(&p->opt, sizeof(p->opt));

        if (ra->rdnss)
                iov[msg.msg_iovlen++] = IOVEC_MAKE(ra->rdnss, ra->rdnss->length * 8);

        if (ra->dnssl)
                iov[msg.msg_iovlen++] = IOVEC_MAKE(ra->dnssl, ra->dnssl->length * 8);

        if (FLAGS_SET(ra->flags, ND_RA_FLAG_HOME_AGENT)) {
                ra->home_agent.nd_opt_home_agent_info_type = ND_OPT_HOME_AGENT_INFO;
                ra->home_agent.nd_opt_home_agent_info_len = 1;

                /* 0 means to place the current Router Lifetime value */
                if (ra->home_agent.nd_opt_home_agent_info_lifetime == 0)
                        ra->home_agent.nd_opt_home_agent_info_lifetime = adv.nd_ra_router_lifetime;

                iov[msg.msg_iovlen++] = IOVEC_MAKE(&ra->home_agent, sizeof(ra->home_agent));
        }

        if (sendmsg(ra->fd, &msg, 0) < 0)
                return -errno;

        return 0;
}

static int radv_recv(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        sd_radv *ra = ASSERT_PTR(userdata);
        struct in6_addr src;
        triple_timestamp timestamp;
        int r;

        assert(s);
        assert(ra->event);

        ssize_t buflen = next_datagram_size_fd(fd);
        if (ERRNO_IS_NEG_TRANSIENT(buflen) || ERRNO_IS_NEG_DISCONNECT(buflen))
                return 0;
        if (buflen < 0) {
                log_radv_errno(ra, buflen, "Failed to determine datagram size to read, ignoring: %m");
                return 0;
        }

        _cleanup_free_ char *buf = new0(char, buflen);
        if (!buf)
                return -ENOMEM;

        r = icmp6_receive(fd, buf, buflen, &src, &timestamp);
        if (ERRNO_IS_NEG_TRANSIENT(r) || ERRNO_IS_NEG_DISCONNECT(r))
                return 0;
        if (r < 0)
                switch (r) {
                case -EADDRNOTAVAIL:
                        log_radv(ra, "Received RS from neither link-local nor null address, ignoring.");
                        return 0;

                case -EMULTIHOP:
                        log_radv(ra, "Received RS with invalid hop limit, ignoring.");
                        return 0;

                case -EPFNOSUPPORT:
                        log_radv(ra, "Received invalid source address from ICMPv6 socket, ignoring.");
                        return 0;

                default:
                        log_radv_errno(ra, r, "Unexpected error receiving from ICMPv6 socket, ignoring: %m");
                        return 0;
                }

        if ((size_t) buflen < sizeof(struct nd_router_solicit)) {
                log_radv(ra, "Too short packet received, ignoring");
                return 0;
        }

        /* TODO: if the sender address is null, check that the message does not have the source link-layer
         * address option. See RFC 4861 Section 6.1.1. */

        const char *addr = IN6_ADDR_TO_STRING(&src);
        r = radv_send_router(ra, &src, ra->lifetime_usec);
        if (r < 0)
                log_radv_errno(ra, r, "Unable to send solicited Router Advertisement to %s, ignoring: %m", addr);
        else
                log_radv(ra, "Sent solicited Router Advertisement to %s.", addr);

        return 0;
}

static int radv_timeout(sd_event_source *s, uint64_t usec, void *userdata) {
        usec_t min_timeout, max_timeout, time_now, timeout;
        sd_radv *ra = ASSERT_PTR(userdata);
        int r;

        assert(s);
        assert(ra->event);
        assert(router_lifetime_is_valid(ra->lifetime_usec));

        r = sd_event_now(ra->event, CLOCK_BOOTTIME, &time_now);
        if (r < 0)
                goto fail;

        r = radv_send_router(ra, NULL, ra->lifetime_usec);
        if (r < 0)
                log_radv_errno(ra, r, "Unable to send Router Advertisement, ignoring: %m");

        /* RFC 4861, Section 6.2.4, sending initial Router Advertisements */
        if (ra->ra_sent < RADV_MAX_INITIAL_RTR_ADVERTISEMENTS)
                max_timeout = RADV_MAX_INITIAL_RTR_ADVERT_INTERVAL_USEC;
        else
                max_timeout = RADV_DEFAULT_MAX_TIMEOUT_USEC;

        /* RFC 4861, Section 6.2.1, lifetime must be at least MaxRtrAdvInterval,
         * so lower the interval here */
        if (ra->lifetime_usec > 0)
                max_timeout = MIN(max_timeout, ra->lifetime_usec);

        if (max_timeout >= 9 * USEC_PER_SEC)
                min_timeout = max_timeout / 3;
        else
                min_timeout = max_timeout * 3 / 4;

        /* RFC 4861, Section 6.2.1.
         * MaxRtrAdvInterval MUST be no less than 4 seconds and no greater than 1800 seconds.
         * MinRtrAdvInterval MUST be no less than 3 seconds and no greater than .75 * MaxRtrAdvInterval. */
        assert(max_timeout >= RADV_MIN_MAX_TIMEOUT_USEC);
        assert(max_timeout <= RADV_MAX_MAX_TIMEOUT_USEC);
        assert(min_timeout >= RADV_MIN_MIN_TIMEOUT_USEC);
        assert(min_timeout <= max_timeout * 3 / 4);

        timeout = min_timeout + random_u64_range(max_timeout - min_timeout);
        log_radv(ra, "Next Router Advertisement in %s", FORMAT_TIMESPAN(timeout, USEC_PER_SEC));

        r = event_reset_time(ra->event, &ra->timeout_event_source,
                             CLOCK_BOOTTIME,
                             usec_add(time_now, timeout), MSEC_PER_SEC,
                             radv_timeout, ra,
                             ra->event_priority, "radv-timeout", true);
        if (r < 0)
                goto fail;

        ra->ra_sent++;

        return 0;

fail:
        sd_radv_stop(ra);

        return 0;
}

int sd_radv_stop(sd_radv *ra) {
        int r;

        if (!ra)
                return 0;

        if (ra->state == RADV_STATE_IDLE)
                return 0;

        log_radv(ra, "Stopping IPv6 Router Advertisement daemon");

        /* RFC 4861, Section 6.2.5:
         * the router SHOULD transmit one or more (but not more than MAX_FINAL_RTR_ADVERTISEMENTS) final
         * multicast Router Advertisements on the interface with a Router Lifetime field of zero. */
        r = radv_send_router(ra, NULL, 0);
        if (r < 0)
                log_radv_errno(ra, r, "Unable to send last Router Advertisement with router lifetime set to zero, ignoring: %m");

        radv_reset(ra);
        ra->fd = safe_close(ra->fd);
        ra->state = RADV_STATE_IDLE;

        return 0;
}

static int radv_setup_recv_event(sd_radv *ra) {
        int r;

        assert(ra);
        assert(ra->event);
        assert(ra->ifindex > 0);

        _cleanup_close_ int fd = -EBADF;
        fd = icmp6_bind(ra->ifindex, /* is_router = */ true);
        if (fd < 0)
                return fd;

        _cleanup_(sd_event_source_unrefp) sd_event_source *s = NULL;
        r = sd_event_add_io(ra->event, &s, fd, EPOLLIN, radv_recv, ra);
        if (r < 0)
                return r;

        r = sd_event_source_set_priority(s, ra->event_priority);
        if (r < 0)
                return r;

        (void) sd_event_source_set_description(s, "radv-receive-message");

        ra->fd = TAKE_FD(fd);
        ra->recv_event_source = TAKE_PTR(s);
        return 0;
}

int sd_radv_start(sd_radv *ra) {
        int r;

        assert_return(ra, -EINVAL);
        assert_return(ra->event, -EINVAL);
        assert_return(ra->ifindex > 0, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return 0;

        r = radv_setup_recv_event(ra);
        if (r < 0)
                goto fail;

        r = event_reset_time(ra->event, &ra->timeout_event_source,
                             CLOCK_BOOTTIME,
                             0, 0,
                             radv_timeout, ra,
                             ra->event_priority, "radv-timeout", true);
        if (r < 0)
                goto fail;

        ra->state = RADV_STATE_ADVERTISING;

        log_radv(ra, "Started IPv6 Router Advertisement daemon");

        return 0;

 fail:
        radv_reset(ra);

        return r;
}

int sd_radv_set_ifindex(sd_radv *ra, int ifindex) {
        assert_return(ra, -EINVAL);
        assert_return(ifindex > 0, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        ra->ifindex = ifindex;

        return 0;
}

int sd_radv_set_ifname(sd_radv *ra, const char *ifname) {
        assert_return(ra, -EINVAL);
        assert_return(ifname, -EINVAL);

        if (!ifname_valid_full(ifname, IFNAME_VALID_ALTERNATIVE))
                return -EINVAL;

        return free_and_strdup(&ra->ifname, ifname);
}

int sd_radv_get_ifname(sd_radv *ra, const char **ret) {
        int r;

        assert_return(ra, -EINVAL);

        r = get_ifname(ra->ifindex, &ra->ifname);
        if (r < 0)
                return r;

        if (ret)
                *ret = ra->ifname;

        return 0;
}

int sd_radv_set_mac(sd_radv *ra, const struct ether_addr *mac_addr) {
        assert_return(ra, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        if (mac_addr)
                ra->mac_addr = *mac_addr;
        else
                zero(ra->mac_addr);

        return 0;
}

int sd_radv_set_mtu(sd_radv *ra, uint32_t mtu) {
        assert_return(ra, -EINVAL);
        assert_return(mtu >= 1280, -EINVAL);

        ra->mtu = mtu;

        return 0;
}

int sd_radv_set_hop_limit(sd_radv *ra, uint8_t hop_limit) {
        assert_return(ra, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        ra->hop_limit = hop_limit;

        return 0;
}

int sd_radv_set_retransmit(sd_radv *ra, uint64_t usec) {
        assert_return(ra, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        if (usec > RADV_MAX_RETRANSMIT_USEC)
                return -EINVAL;

        ra->retransmit_usec = usec;
        return 0;
}

int sd_radv_set_router_lifetime(sd_radv *ra, uint64_t usec) {
        assert_return(ra, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        if (!router_lifetime_is_valid(usec))
                return -EINVAL;

        /* RFC 4191, Section 2.2, "...If the Router Lifetime is zero, the preference value MUST be set
         * to (00) by the sender..." */
        if (usec == 0 &&
            (ra->flags & (0x3 << 3)) != (SD_NDISC_PREFERENCE_MEDIUM << 3))
                return -EINVAL;

        ra->lifetime_usec = usec;
        return 0;
}

int sd_radv_set_managed_information(sd_radv *ra, int managed) {
        assert_return(ra, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        SET_FLAG(ra->flags, ND_RA_FLAG_MANAGED, managed);

        return 0;
}

int sd_radv_set_other_information(sd_radv *ra, int other) {
        assert_return(ra, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        SET_FLAG(ra->flags, ND_RA_FLAG_OTHER, other);

        return 0;
}

int sd_radv_set_preference(sd_radv *ra, unsigned preference) {
        assert_return(ra, -EINVAL);
        assert_return(IN_SET(preference,
                             SD_NDISC_PREFERENCE_LOW,
                             SD_NDISC_PREFERENCE_MEDIUM,
                             SD_NDISC_PREFERENCE_HIGH), -EINVAL);

        /* RFC 4191, Section 2.2, "...If the Router Lifetime is zero, the preference value MUST be set
         * to (00) by the sender..." */
        if (ra->lifetime_usec == 0 && preference != SD_NDISC_PREFERENCE_MEDIUM)
                return -EINVAL;

        ra->flags = (ra->flags & ~(0x3 << 3)) | (preference << 3);

        return 0;
}

int sd_radv_set_home_agent_information(sd_radv *ra, int home_agent) {
        assert_return(ra, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        SET_FLAG(ra->flags, ND_RA_FLAG_HOME_AGENT, home_agent);

        return 0;
}

int sd_radv_set_home_agent_preference(sd_radv *ra, uint16_t preference) {
        assert_return(ra, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        ra->home_agent.nd_opt_home_agent_info_preference = htobe16(preference);

        return 0;
}

int sd_radv_set_home_agent_lifetime(sd_radv *ra, uint64_t lifetime_usec) {
        assert_return(ra, -EINVAL);

        if (ra->state != RADV_STATE_IDLE)
                return -EBUSY;

        if (lifetime_usec > RADV_HOME_AGENT_MAX_LIFETIME_USEC)
                return -EINVAL;

        ra->home_agent.nd_opt_home_agent_info_lifetime = usec_to_be16_sec(lifetime_usec);
        return 0;
}

int sd_radv_add_prefix(sd_radv *ra, sd_radv_prefix *p) {
        sd_radv_prefix *found = NULL;
        int r;

        assert_return(ra, -EINVAL);
        assert_return(p, -EINVAL);

        /* Refuse prefixes that don't have a prefix set */
        if (in6_addr_is_null(&p->opt.in6_addr))
                return -ENOEXEC;

        const char *addr_p = IN6_ADDR_PREFIX_TO_STRING(&p->opt.in6_addr, p->opt.prefixlen);

        LIST_FOREACH(prefix, cur, ra->prefixes) {
                r = in_addr_prefix_intersect(AF_INET6,
                                             (const union in_addr_union*) &cur->opt.in6_addr,
                                             cur->opt.prefixlen,
                                             (const union in_addr_union*) &p->opt.in6_addr,
                                             p->opt.prefixlen);
                if (r < 0)
                        return r;
                if (r == 0)
                        continue;

                if (cur->opt.prefixlen == p->opt.prefixlen) {
                        found = cur;
                        break;
                }

                return log_radv_errno(ra, SYNTHETIC_ERRNO(EEXIST),
                                      "IPv6 prefix %s conflicts with %s, ignoring.",
                                      addr_p,
                                      IN6_ADDR_PREFIX_TO_STRING(&cur->opt.in6_addr, cur->opt.prefixlen));
        }

        if (found) {
                /* p and cur may be equivalent. First increment the reference counter. */
                sd_radv_prefix_ref(p);

                /* Then, remove the old entry. */
                LIST_REMOVE(prefix, ra->prefixes, found);
                sd_radv_prefix_unref(found);

                /* Finally, add the new entry. */
                LIST_APPEND(prefix, ra->prefixes, p);

                log_radv(ra, "Updated/replaced IPv6 prefix %s (preferred: %s, valid: %s)",
                         addr_p,
                         FORMAT_TIMESPAN(p->lifetime_preferred_usec, USEC_PER_SEC),
                         FORMAT_TIMESPAN(p->lifetime_valid_usec, USEC_PER_SEC));
        } else {
                /* The prefix is new. Let's simply add it. */

                sd_radv_prefix_ref(p);
                LIST_APPEND(prefix, ra->prefixes, p);
                ra->n_prefixes++;

                log_radv(ra, "Added prefix %s", addr_p);
        }

        if (ra->state == RADV_STATE_IDLE)
                return 0;

        if (ra->ra_sent == 0)
                return 0;

        /* If RAs have already been sent, send an RA immediately to announce the newly-added prefix */
        r = radv_send_router(ra, NULL, ra->lifetime_usec);
        if (r < 0)
                log_radv_errno(ra, r, "Unable to send Router Advertisement for added prefix %s, ignoring: %m", addr_p);
        else
                log_radv(ra, "Sent Router Advertisement for added/updated prefix %s.", addr_p);

        return 0;
}

void sd_radv_remove_prefix(
                sd_radv *ra,
                const struct in6_addr *prefix,
                unsigned char prefixlen) {

        if (!ra)
                return;

        if (!prefix)
                return;

        LIST_FOREACH(prefix, cur, ra->prefixes) {
                if (prefixlen != cur->opt.prefixlen)
                        continue;

                if (!in6_addr_equal(prefix, &cur->opt.in6_addr))
                        continue;

                LIST_REMOVE(prefix, ra->prefixes, cur);
                ra->n_prefixes--;
                sd_radv_prefix_unref(cur);
                return;
        }
}

int sd_radv_add_route_prefix(sd_radv *ra, sd_radv_route_prefix *p) {
        sd_radv_route_prefix *found = NULL;
        int r;

        assert_return(ra, -EINVAL);
        assert_return(p, -EINVAL);

        const char *addr_p = IN6_ADDR_PREFIX_TO_STRING(&p->opt.in6_addr, p->opt.prefixlen);

        LIST_FOREACH(prefix, cur, ra->route_prefixes) {
                r = in_addr_prefix_intersect(AF_INET6,
                                             (const union in_addr_union*) &cur->opt.in6_addr,
                                             cur->opt.prefixlen,
                                             (const union in_addr_union*) &p->opt.in6_addr,
                                             p->opt.prefixlen);
                if (r < 0)
                        return r;
                if (r == 0)
                        continue;

                if (cur->opt.prefixlen == p->opt.prefixlen) {
                        found = cur;
                        break;
                }

                return log_radv_errno(ra, SYNTHETIC_ERRNO(EEXIST),
                                      "IPv6 route prefix %s conflicts with %s, ignoring.",
                                      addr_p,
                                      IN6_ADDR_PREFIX_TO_STRING(&cur->opt.in6_addr, cur->opt.prefixlen));
        }

        if (found) {
                /* p and cur may be equivalent. First increment the reference counter. */
                sd_radv_route_prefix_ref(p);

                /* Then, remove the old entry. */
                LIST_REMOVE(prefix, ra->route_prefixes, found);
                sd_radv_route_prefix_unref(found);

                /* Finally, add the new entry. */
                LIST_APPEND(prefix, ra->route_prefixes, p);

                log_radv(ra, "Updated/replaced IPv6 route prefix %s (lifetime: %s)",
                         strna(addr_p),
                         FORMAT_TIMESPAN(p->lifetime_usec, USEC_PER_SEC));
        } else {
                /* The route prefix is new. Let's simply add it. */

                sd_radv_route_prefix_ref(p);
                LIST_APPEND(prefix, ra->route_prefixes, p);
                ra->n_route_prefixes++;

                log_radv(ra, "Added route prefix %s", strna(addr_p));
        }

        if (ra->state == RADV_STATE_IDLE)
                return 0;

        if (ra->ra_sent == 0)
                return 0;

        /* If RAs have already been sent, send an RA immediately to announce the newly-added route prefix */
        r = radv_send_router(ra, NULL, ra->lifetime_usec);
        if (r < 0)
                log_radv_errno(ra, r, "Unable to send Router Advertisement for added route prefix %s, ignoring: %m",
                               strna(addr_p));
        else
                log_radv(ra, "Sent Router Advertisement for added route prefix %s.", strna(addr_p));

        return 0;
}

int sd_radv_add_pref64_prefix(sd_radv *ra, sd_radv_pref64_prefix *p) {
        sd_radv_pref64_prefix *found = NULL;
        int r;

        assert_return(ra, -EINVAL);
        assert_return(p, -EINVAL);

        const char *addr_p = IN6_ADDR_PREFIX_TO_STRING(&p->in6_addr, p->prefixlen);

        LIST_FOREACH(prefix, cur, ra->pref64_prefixes) {
                r = in_addr_prefix_intersect(AF_INET6,
                                             (const union in_addr_union*) &cur->in6_addr,
                                             cur->prefixlen,
                                             (const union in_addr_union*) &p->in6_addr,
                                             p->prefixlen);
                if (r < 0)
                        return r;
                if (r == 0)
                        continue;

                if (cur->prefixlen == p->prefixlen) {
                        found = cur;
                        break;
                }

                return log_radv_errno(ra, SYNTHETIC_ERRNO(EEXIST),
                                      "IPv6 PREF64 prefix %s conflicts with %s, ignoring.",
                                      addr_p,
                                      IN6_ADDR_PREFIX_TO_STRING(&cur->in6_addr, cur->prefixlen));
        }

        if (found) {
                /* p and cur may be equivalent. First increment the reference counter. */
                sd_radv_pref64_prefix_ref(p);

                /* Then, remove the old entry. */
                LIST_REMOVE(prefix, ra->pref64_prefixes, found);
                sd_radv_pref64_prefix_unref(found);

                /* Finally, add the new entry. */
                LIST_APPEND(prefix, ra->pref64_prefixes, p);

                log_radv(ra, "Updated/replaced IPv6 PREF64 prefix %s (lifetime: %s)",
                         strna(addr_p),
                         FORMAT_TIMESPAN(p->lifetime_usec, USEC_PER_SEC));
        } else {
                /* The route prefix is new. Let's simply add it. */

                sd_radv_pref64_prefix_ref(p);
                LIST_APPEND(prefix, ra->pref64_prefixes, p);
                ra->n_pref64_prefixes++;

                log_radv(ra, "Added PREF64 prefix %s", strna(addr_p));
        }

        if (ra->state == RADV_STATE_IDLE)
                return 0;

        if (ra->ra_sent == 0)
                return 0;

        /* If RAs have already been sent, send an RA immediately to announce the newly-added route prefix */
        r = radv_send_router(ra, NULL, ra->lifetime_usec);
        if (r < 0)
                log_radv_errno(ra, r, "Unable to send Router Advertisement for added PREF64 prefix %s, ignoring: %m",
                               strna(addr_p));
        else
                log_radv(ra, "Sent Router Advertisement for added PREF64 prefix %s.", strna(addr_p));

        return 0;
}

int sd_radv_set_rdnss(
                sd_radv *ra,
                uint64_t lifetime_usec,
                const struct in6_addr *dns,
                size_t n_dns) {

        _cleanup_free_ struct sd_radv_opt_dns *opt_rdnss = NULL;
        size_t len;

        assert_return(ra, -EINVAL);
        assert_return(n_dns < 128, -EINVAL);

        if (lifetime_usec > RADV_RDNSS_MAX_LIFETIME_USEC)
                return -EINVAL;

        if (!dns || n_dns == 0) {
                ra->rdnss = mfree(ra->rdnss);
                ra->n_rdnss = 0;

                return 0;
        }

        len = sizeof(struct sd_radv_opt_dns) + sizeof(struct in6_addr) * n_dns;

        opt_rdnss = malloc0(len);
        if (!opt_rdnss)
                return -ENOMEM;

        opt_rdnss->type = RADV_OPT_RDNSS;
        opt_rdnss->length = len / 8;
        opt_rdnss->lifetime = usec_to_be32_sec(lifetime_usec);

        memcpy(opt_rdnss + 1, dns, n_dns * sizeof(struct in6_addr));

        free_and_replace(ra->rdnss, opt_rdnss);

        ra->n_rdnss = n_dns;

        return 0;
}

int sd_radv_set_dnssl(
                sd_radv *ra,
                uint64_t lifetime_usec,
                char **search_list) {

        _cleanup_free_ struct sd_radv_opt_dns *opt_dnssl = NULL;
        size_t len = 0;
        uint8_t *p;

        assert_return(ra, -EINVAL);

        if (lifetime_usec > RADV_DNSSL_MAX_LIFETIME_USEC)
                return -EINVAL;

        if (strv_isempty(search_list)) {
                ra->dnssl = mfree(ra->dnssl);
                return 0;
        }

        STRV_FOREACH(s, search_list)
                len += strlen(*s) + 2;

        len = (sizeof(struct sd_radv_opt_dns) + len + 7) & ~0x7;

        opt_dnssl = malloc0(len);
        if (!opt_dnssl)
                return -ENOMEM;

        opt_dnssl->type = RADV_OPT_DNSSL;
        opt_dnssl->length = len / 8;
        opt_dnssl->lifetime = usec_to_be32_sec(lifetime_usec);

        p = (uint8_t *)(opt_dnssl + 1);
        len -= sizeof(struct sd_radv_opt_dns);

        STRV_FOREACH(s, search_list) {
                int r;

                r = dns_name_to_wire_format(*s, p, len, false);
                if (r < 0)
                        return r;

                if (len < (size_t)r)
                        return -ENOBUFS;

                p += r;
                len -= r;
        }

        free_and_replace(ra->dnssl, opt_dnssl);

        return 0;
}

int sd_radv_prefix_new(sd_radv_prefix **ret) {
        sd_radv_prefix *p;

        assert_return(ret, -EINVAL);

        p = new(sd_radv_prefix, 1);
        if (!p)
                return -ENOMEM;

        *p = (sd_radv_prefix) {
                .n_ref = 1,

                .opt.type = ND_OPT_PREFIX_INFORMATION,
                .opt.length = (sizeof(p->opt) - 1)/8 + 1,
                .opt.prefixlen = 64,

                /* RFC 4861, Section 6.2.1 */
                .opt.flags = ND_OPT_PI_FLAG_ONLINK|ND_OPT_PI_FLAG_AUTO,

                .lifetime_valid_usec = RADV_DEFAULT_VALID_LIFETIME_USEC,
                .lifetime_preferred_usec = RADV_DEFAULT_PREFERRED_LIFETIME_USEC,
                .valid_until = USEC_INFINITY,
                .preferred_until = USEC_INFINITY,
        };

        *ret = p;
        return 0;
}

DEFINE_PUBLIC_TRIVIAL_REF_UNREF_FUNC(sd_radv_prefix, sd_radv_prefix, mfree);

int sd_radv_prefix_set_prefix(
                sd_radv_prefix *p,
                const struct in6_addr *in6_addr,
                unsigned char prefixlen) {

        assert_return(p, -EINVAL);
        assert_return(in6_addr, -EINVAL);

        if (prefixlen < 3 || prefixlen > 128)
                return -EINVAL;

        if (prefixlen > 64)
                /* unusual but allowed, log it */
                log_radv(NULL, "Unusual prefix length %d greater than 64", prefixlen);

        p->opt.in6_addr = *in6_addr;
        p->opt.prefixlen = prefixlen;

        return 0;
}

int sd_radv_prefix_get_prefix(
                sd_radv_prefix *p,
                struct in6_addr *ret_in6_addr,
                unsigned char *ret_prefixlen) {

        assert_return(p, -EINVAL);
        assert_return(ret_in6_addr, -EINVAL);
        assert_return(ret_prefixlen, -EINVAL);

        *ret_in6_addr = p->opt.in6_addr;
        *ret_prefixlen = p->opt.prefixlen;

        return 0;
}

int sd_radv_prefix_set_onlink(sd_radv_prefix *p, int onlink) {
        assert_return(p, -EINVAL);

        SET_FLAG(p->opt.flags, ND_OPT_PI_FLAG_ONLINK, onlink);

        return 0;
}

int sd_radv_prefix_set_address_autoconfiguration(sd_radv_prefix *p, int address_autoconfiguration) {
        assert_return(p, -EINVAL);

        SET_FLAG(p->opt.flags, ND_OPT_PI_FLAG_AUTO, address_autoconfiguration);

        return 0;
}

int sd_radv_prefix_set_valid_lifetime(sd_radv_prefix *p, uint64_t lifetime_usec, uint64_t valid_until) {
        assert_return(p, -EINVAL);

        p->lifetime_valid_usec = lifetime_usec;
        p->valid_until = valid_until;

        return 0;
}

int sd_radv_prefix_set_preferred_lifetime(sd_radv_prefix *p, uint64_t lifetime_usec, uint64_t valid_until) {
        assert_return(p, -EINVAL);

        p->lifetime_preferred_usec = lifetime_usec;
        p->preferred_until = valid_until;

        return 0;
}

int sd_radv_route_prefix_new(sd_radv_route_prefix **ret) {
        sd_radv_route_prefix *p;

        assert_return(ret, -EINVAL);

        p = new(sd_radv_route_prefix, 1);
        if (!p)
                return -ENOMEM;

        *p = (sd_radv_route_prefix) {
                .n_ref = 1,

                .opt.type = RADV_OPT_ROUTE_INFORMATION,
                .opt.length = DIV_ROUND_UP(sizeof(p->opt), 8),
                .opt.prefixlen = 64,

                .lifetime_usec = RADV_DEFAULT_VALID_LIFETIME_USEC,
                .valid_until = USEC_INFINITY,
        };

        *ret = p;
        return 0;
}

DEFINE_PUBLIC_TRIVIAL_REF_UNREF_FUNC(sd_radv_route_prefix, sd_radv_route_prefix, mfree);

int sd_radv_route_prefix_set_prefix(
                sd_radv_route_prefix *p,
                const struct in6_addr *in6_addr,
                unsigned char prefixlen) {

        assert_return(p, -EINVAL);
        assert_return(in6_addr, -EINVAL);

        if (prefixlen > 128)
                return -EINVAL;

        if (prefixlen > 64)
                /* unusual but allowed, log it */
                log_radv(NULL, "Unusual prefix length %u greater than 64", prefixlen);

        p->opt.in6_addr = *in6_addr;
        p->opt.prefixlen = prefixlen;

        return 0;
}

int sd_radv_route_prefix_set_lifetime(sd_radv_route_prefix *p, uint64_t lifetime_usec, uint64_t valid_until) {
        assert_return(p, -EINVAL);

        p->lifetime_usec = lifetime_usec;
        p->valid_until = valid_until;

        return 0;
}

int sd_radv_pref64_prefix_new(sd_radv_pref64_prefix **ret) {
        sd_radv_pref64_prefix *p;

        assert_return(ret, -EINVAL);

        p = new(sd_radv_pref64_prefix, 1);
        if (!p)
                return -ENOMEM;

        *p = (sd_radv_pref64_prefix) {
                .n_ref = 1,

                .opt.type = RADV_OPT_PREF64,
                .opt.length = 2,
        };

        *ret = p;
        return 0;
}

DEFINE_PUBLIC_TRIVIAL_REF_UNREF_FUNC(sd_radv_pref64_prefix, sd_radv_pref64_prefix, mfree);

int sd_radv_pref64_prefix_set_prefix(
                sd_radv_pref64_prefix *p,
                const struct in6_addr *prefix,
                uint8_t prefixlen,
                uint64_t lifetime_usec) {

        uint16_t pref64_lifetime;
        uint8_t prefixlen_code;
        int r;

        assert_return(p, -EINVAL);
        assert_return(prefix, -EINVAL);

        r = pref64_prefix_length_to_plc(prefixlen, &prefixlen_code);
        if (r < 0)
                return log_radv_errno(NULL, r,
                                      "Unsupported PREF64 prefix length %u. Valid lengths are 32, 40, 48, 56, 64 and 96", prefixlen);

        if (lifetime_usec > PREF64_MAX_LIFETIME_USEC)
                return -EINVAL;

        /* RFC 8781 - 4.1 rounding up lifetime to multiply of 8 */
        pref64_lifetime = DIV_ROUND_UP(lifetime_usec, 8 * USEC_PER_SEC) << 3;
        pref64_lifetime |= prefixlen_code;

        unaligned_write_be16(&p->opt.lifetime_and_plc, pref64_lifetime);
        memcpy(&p->opt.prefix, prefix, sizeof(p->opt.prefix));

        p->in6_addr = *prefix;
        p->prefixlen = prefixlen;

        return 0;
}
