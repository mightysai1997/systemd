/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "hashmap.h"

typedef enum FilesystemParseFlags {
        FILESYSTEM_PARSE_INVERT     = 1 << 0,
        FILESYSTEM_PARSE_ALLOW_LIST = 1 << 1,
        FILESYSTEM_PARSE_LOG        = 1 << 2,
} FilesystemParseFlags;

typedef struct Unit Unit;
typedef struct Manager Manager;

typedef struct restrict_fs_bpf restrict_fs_bpf;

int lsm_bpf_supported(void);
int lsm_bpf_setup(Manager *m);
int bpf_restrict_filesystems(const Set *filesystems, const bool allow_list, Unit *u);
int cleanup_lsm_bpf(const Unit *u);
int bpf_map_restrict_fs_fd(Unit *u);
void lsm_bpf_destroy(struct restrict_fs_bpf *prog);
int bpf_lsm_parse_filesystem(const char *name,
                             Set *filesystems,
                             FilesystemParseFlags flags,
                             const char *unit,
                             const char *filename,
                             unsigned line);
