/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <linux/if.h>

#include "sd-event.h"
#include "sd-lldp.h"

#include "hashmap.h"
#include "log-link.h"
#include "prioq.h"

struct sd_lldp {
        unsigned n_ref;

        int ifindex;
        char ifname[IFNAMSIZ + 1];
        int fd;

        sd_event *event;
        int64_t event_priority;
        sd_event_source *io_event_source;
        sd_event_source *timer_event_source;

        Prioq *neighbor_by_expiry;
        Hashmap *neighbor_by_id;

        uint64_t neighbors_max;

        sd_lldp_callback_t callback;
        void *userdata;

        uint16_t capability_mask;

        struct ether_addr filter_address;
};

const char* lldp_event_to_string(sd_lldp_event_t e) _const_;
sd_lldp_event_t lldp_event_from_string(const char *s) _pure_;

#define log_lldp_errno(lldp, error, fmt, ...)          \
        log_interface_full_errno(sd_lldp_get_ifname(lldp), LOG_DEBUG, error, "LLDP: " fmt, ##__VA_ARGS__)
#define log_lldp(lldp, fmt, ...)                       \
        log_lldp_errno(lldp, 0, fmt, ##__VA_ARGS__)
