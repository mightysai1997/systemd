/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

/***
  Copyright © 2016 BISDN GmbH. All rights reserved.
***/

#include <inttypes.h>
#include <stdbool.h>

#include "sd-netlink.h"

#include "conf-parser.h"

#define BRIDGE_VLAN_BITMAP_MAX 4096
#define BRIDGE_VLAN_BITMAP_LEN (BRIDGE_VLAN_BITMAP_MAX / 32)

typedef struct Link Link;
typedef struct Network Network;

void network_adjust_bridge_vlan(Network *network);

int link_update_bridge_vlan(Link *link, sd_netlink_message *m);

int bridge_vlan_set_message(Link *link, sd_netlink_message *m, bool is_set);

CONFIG_PARSER_PROTOTYPE(config_parse_bridge_vlan_id);
CONFIG_PARSER_PROTOTYPE(config_parse_bridge_vlan_id_range);
