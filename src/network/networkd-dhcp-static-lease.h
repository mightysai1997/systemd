/* SPDX-License-Identifier: LGPL-2.1+ */

#pragma once

#include <inttypes.h>
#include <stdbool.h>

#include "sd-dhcp-static-lease.h"

#include "conf-parser.h"
#include "networkd-util.h"

typedef struct DHCPStaticLease DHCPStaticLease;
typedef struct Network Network;
typedef struct NetworkConfigSection NetworkConfigSection;

struct DHCPStaticLease {
        Network *network;
        NetworkConfigSection *section;

        sd_dhcp_static_lease *static_lease;
};

DHCPStaticLease *dhcp_static_lease_free(DHCPStaticLease *lease);
void network_drop_invalid_static_leases(Network *network);

CONFIG_PARSER_PROTOTYPE(config_parse_dhcp_static_lease_address);
CONFIG_PARSER_PROTOTYPE(config_parse_dhcp_static_lease_hwaddr);
