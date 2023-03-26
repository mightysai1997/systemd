/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "sd-id128.h"

typedef enum BootEntryTokenType {
        BOOT_ENTRY_TOKEN_MACHINE_ID,
        BOOT_ENTRY_TOKEN_OS_IMAGE_ID,
        BOOT_ENTRY_TOKEN_OS_ID,
        BOOT_ENTRY_TOKEN_LITERAL,
        BOOT_ENTRY_TOKEN_AUTO,
} BootEntryTokenType;

bool boot_entry_token_valid(const char *p);

int boot_entry_token_from_type(
                const char *root,
                const char *conf_root, /* will be prefixed with root */
                sd_id128_t machine_id,
                BootEntryTokenType *type,
                char **ret);

int parse_boot_entry_token_type(const char *s, BootEntryTokenType *type, char **token);
