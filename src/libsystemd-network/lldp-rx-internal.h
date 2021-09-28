/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "sd-event.h"
#include "sd-lldp-rx.h"

#include "hashmap.h"
#include "log-link.h"
#include "prioq.h"

struct sd_lldp_rx {
        unsigned n_ref;

        int ifindex;
        char *ifname;
        int fd;

        sd_event *event;
        int64_t event_priority;
        sd_event_source *io_event_source;
        sd_event_source *timer_event_source;

        Prioq *neighbor_by_expiry;
        Hashmap *neighbor_by_id;

        uint64_t neighbors_max;

        sd_lldp_rx_callback_t callback;
        void *userdata;

        uint16_t capability_mask;

        struct ether_addr filter_address;
};

const char* lldp_rx_event_to_string(sd_lldp_rx_event_t e) _const_;
sd_lldp_rx_event_t lldp_rx_event_from_string(const char *s) _pure_;

#define log_lldp_rx_errno(lldp_rx, error, fmt, ...)     \
        ({                                              \
                sd_lldp_rx *_l = (lldp_rx);             \
                const char *_n = NULL;                  \
                                                        \
                (void) sd_lldp_rx_get_ifname(_l, &_n);  \
                log_interface_prefix_full_errno(        \
                        "LLDP Rx: ",                    \
                        _n, error, fmt, ##__VA_ARGS__); \
        })
#define log_lldp_rx(lldp_rx, fmt, ...)                  \
        ({                                              \
                sd_lldp_rx *_l = (lldp_rx);             \
                const char *_n = NULL;                  \
                                                        \
                (void) sd_lldp_rx_get_ifname(_l, &_n);  \
                log_interface_prefix_full_errno_zerook( \
                        "LLDP Rx: ",                    \
                        _n, 0, fmt, ##__VA_ARGS__);     \
        })
