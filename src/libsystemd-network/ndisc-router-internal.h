/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

/***
  Copyright © 2014 Intel Corporation. All rights reserved.
***/

#include "sd-ndisc.h"

#include "icmp6-packet.h"
#include "time-util.h"

struct sd_ndisc_router {
        unsigned n_ref;

        ICMP6Packet *packet;

        /* The current read index for the iterative option interface */
        size_t rindex;

        uint64_t flags;
        unsigned preference;
        uint64_t lifetime_usec;
        usec_t reachable_time_usec;
        usec_t retransmission_time_usec;

        uint8_t hop_limit;
        uint32_t mtu;
        uint64_t icmp6_ratelimit_usec;
};

sd_ndisc_router* ndisc_router_new(ICMP6Packet *packet);
int ndisc_router_parse(sd_ndisc *nd, sd_ndisc_router *rt);
