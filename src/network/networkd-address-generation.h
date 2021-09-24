/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "conf-parser.h"
#include "in-addr-util.h"
#include "set.h"

typedef struct Link Link;

void generate_eui64_address(const Link *link, const struct in6_addr *prefix, struct in6_addr *ret);

int dhcp6_pd_generate_addresses(Link *link, const struct in6_addr *prefix, Set **ret);
int ndisc_generate_addresses(Link *link, const struct in6_addr *prefix, uint8_t prefixlen, Set **ret);

CONFIG_PARSER_PROTOTYPE(config_parse_address_generation_type);
