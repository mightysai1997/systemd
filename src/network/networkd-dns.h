/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "conf-parser.h"
#include "macro.h"

typedef enum UseDomains {
        USE_DOMAINS_NO,
        USE_DOMAINS_YES,
        USE_DOMAINS_ROUTE,
        _USE_DOMAINS_MAX,
        _USE_DOMAINS_INVALID = -EINVAL,
} UseDomains;

const char* use_domains_to_string(UseDomains p) _const_;
UseDomains use_domains_from_string(const char *s) _pure_;

CONFIG_PARSER_PROTOTYPE(config_parse_domains);
CONFIG_PARSER_PROTOTYPE(config_parse_dns);
CONFIG_PARSER_PROTOTYPE(config_parse_dnssec_negative_trust_anchors);
CONFIG_PARSER_PROTOTYPE(config_parse_dhcp_use_dns);
CONFIG_PARSER_PROTOTYPE(config_parse_dhcp_use_domains);
CONFIG_PARSER_PROTOTYPE(config_parse_use_domains);
