/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "conf-parser.h"
#include "set.h"

typedef struct Address Address;

void address_add_netlabel(const Address *address);
void address_del_netlabel(const Address *address);

CONFIG_PARSER_PROTOTYPE(config_parse_netlabel);
