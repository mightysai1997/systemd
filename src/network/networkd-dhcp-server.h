/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "conf-parser.h"
#include "set.h"

typedef struct Link Link;
typedef struct Network Network;

bool dhcp4_server_can_start(void);

int network_adjust_dhcp_server(Network *network, Set **addresses);

bool link_dhcp4_server_enabled(Link *link);
int link_request_dhcp_server(Link *link);
bool link_dhcp4_server_is_ready_to_start(Link *link);
int link_start_dhcp4_server(Link *link);
int link_toggle_dhcp4_server_state(Link *link, bool start);

CONFIG_PARSER_PROTOTYPE(config_parse_dhcp_server_relay_agent_suboption);
CONFIG_PARSER_PROTOTYPE(config_parse_dhcp_server_emit);
CONFIG_PARSER_PROTOTYPE(config_parse_dhcp_server_address);
CONFIG_PARSER_PROTOTYPE(config_parse_dhcp_server_ipv6_only_preferred);
