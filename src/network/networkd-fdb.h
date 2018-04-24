/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

/***
  This file is part of systemd.

  Copyright (C) 2014 Intel Corporation. All rights reserved.
***/

#include "list.h"
#include "macro.h"

typedef struct Network Network;
typedef struct FdbEntry FdbEntry;
typedef struct Link Link;

struct FdbEntry {
        Network *network;
        unsigned section;

        struct ether_addr *mac_addr;
        uint16_t vlan_id;

        LIST_FIELDS(FdbEntry, static_fdb_entries);
};

int fdb_entry_new_static(Network *network, unsigned section, FdbEntry **ret);
void fdb_entry_free(FdbEntry *fdb_entry);
int fdb_entry_configure(Link *link, FdbEntry *fdb_entry);

DEFINE_TRIVIAL_CLEANUP_FUNC(FdbEntry*, fdb_entry_free);

int config_parse_fdb_hwaddr(const char *unit, const char *filename, unsigned line, const char *section, unsigned section_line, const char *lvalue, int ltype, const char *rvalue, void *data, void *userdata);
int config_parse_fdb_vlan_id(const char *unit, const char *filename, unsigned line, const char *section, unsigned section_line, const char *lvalue, int ltype, const char *rvalue, void *data, void *userdata);
