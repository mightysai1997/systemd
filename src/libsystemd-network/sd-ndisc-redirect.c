/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <netinet/icmp6.h>

#include "sd-ndisc.h"

#include "alloc-util.h"
#include "ether-addr-util.h"
#include "memory-util.h"
#include "ndisc-internal.h"
#include "ndisc-protocol.h"
#include "ndisc-redirect-internal.h"

static sd_ndisc_redirect* ndisc_redirect_free(sd_ndisc_redirect *rd) {
        if (!rd)
                return NULL;

        icmp6_packet_unref(rd->packet);
        return mfree(rd);
}

DEFINE_PUBLIC_TRIVIAL_REF_UNREF_FUNC(sd_ndisc_redirect, sd_ndisc_redirect, ndisc_redirect_free);

sd_ndisc_redirect* ndisc_redirect_new(ICMP6Packet *packet) {
        sd_ndisc_redirect *rd;

        assert(packet);

        rd = new(sd_ndisc_redirect, 1);
        if (!rd)
                return NULL;

        *rd = (sd_ndisc_redirect) {
                .n_ref = 1,
                .packet = icmp6_packet_ref(packet),
        };

        return rd;
}

int ndisc_redirect_parse(sd_ndisc *nd, sd_ndisc_redirect *rd) {
        int r;

        assert(rd);
        assert(rd->packet);

        if (rd->packet->raw_size < sizeof(struct nd_redirect))
                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                       "Too small to be a redirect message, ignoring.");

        const struct nd_redirect *a = (const struct nd_redirect*) rd->packet->raw_packet;
        assert(a->nd_rd_type == ND_REDIRECT);
        assert(a->nd_rd_code == 0);

        rd->target_address = a->nd_rd_target;
        rd->destination_address = a->nd_rd_dst;

        for (size_t offset = sizeof(struct nd_redirect), length; offset < rd->packet->raw_size; offset += length) {
                uint8_t type;
                const uint8_t *p;

                r = ndisc_option_parse(rd->packet, offset, &type, &length, &p);
                if (r < 0)
                        return log_ndisc_errno(nd, r, "Failed to parse NDisc option header, ignoring: %m");

                switch (type) {

                case SD_NDISC_OPTION_TARGET_LL_ADDRESS:
                        if (length != sizeof(struct ether_addr) + 2)
                                return log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                       "Redirect message target link-layer address option with invalid length, ignoring datagram.");

                        memcpy(&rd->target_mac, p + 2, sizeof(struct ether_addr));
                        break;

                case SD_NDISC_OPTION_REDIRECTED_HEADER:
                        if (length < sizeof(struct nd_opt_rd_hdr) + sizeof(struct ip6_hdr)) {
                                log_ndisc_errno(nd, SYNTHETIC_ERRNO(EBADMSG),
                                                "Redirected header option with invalid length, ignoring the option.");
                                continue;
                        }

                        /* For safety, here we copy only IPv6 header. */
                        rd->redirected_header = *(struct ip6_hdr*) (p + sizeof(struct nd_opt_rd_hdr));
                        break;
                }
        }

        return 0;
}

int sd_ndisc_redirect_get_sender_address(sd_ndisc_redirect *rd, struct in6_addr *ret) {
        assert_return(rd, -EINVAL);
        assert_return(ret, -EINVAL);

        return icmp6_packet_get_sender_address(rd->packet, ret);
}

int sd_ndisc_redirect_get_target_address(sd_ndisc_redirect *rd, struct in6_addr *ret) {
        assert_return(rd, -EINVAL);
        assert_return(ret, -EINVAL);

        if (in6_addr_is_null(&rd->target_address))
                return -ENODATA;

        *ret = rd->target_address;
        return 0;
}

int sd_ndisc_redirect_get_destination_address(sd_ndisc_redirect *rd, struct in6_addr *ret) {
        assert_return(rd, -EINVAL);
        assert_return(ret, -EINVAL);

        if (in6_addr_is_null(&rd->destination_address))
                return -ENODATA;

        *ret = rd->destination_address;
        return 0;
}

int sd_ndisc_redirect_get_target_mac(sd_ndisc_redirect *rd, struct ether_addr *ret) {
        assert_return(rd, -EINVAL);
        assert_return(ret, -EINVAL);

        if (ether_addr_is_null(&rd->target_mac))
                return -ENODATA;

        *ret = rd->target_mac;
        return 0;
}

int sd_ndisc_redirect_get_redirected_header(sd_ndisc_redirect *rd, struct ip6_hdr *ret) {
        assert_return(rd, -EINVAL);
        assert_return(ret, -EINVAL);

        if (rd->redirected_header.ip6_vfc == 0)
                return -ENODATA;

        *ret = rd->redirected_header;
        return 0;
}
