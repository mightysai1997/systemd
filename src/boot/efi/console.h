/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "missing_efi.h"

#define EFI_CONTROL_PRESSED             (EFI_RIGHT_CONTROL_PRESSED|EFI_LEFT_CONTROL_PRESSED)
#define EFI_ALT_PRESSED                 (EFI_RIGHT_ALT_PRESSED|EFI_LEFT_ALT_PRESSED)
#define KEYPRESS(keys, scan, uni) ((((UINT64)keys) << 32) | (((UINT64)scan) << 16) | (uni))
#define KEYCHAR(k) ((k) & 0xffff)
#define CHAR_CTRL(c) ((c) - 'a' + 1)

enum console_mode_special {
        CONSOLE_MODE_MIN = 0,
        CONSOLE_MODE_MAX = INT32_MAX,      /* This is just the theoretical limit. */
        CONSOLE_MODE_INVALID = (UINT32)-1, /* UEFI indicates -1 if the device is not in a valid text mode. */

        CONSOLE_MODE_80_25 = 0U,      /* 80x25 is required by UEFI spec. */
        CONSOLE_MODE_80_50 = 1U,      /* 80x50 may be supported. */
        CONSOLE_MODE_IMPL_FIRST = 2U, /* First custom mode supported by device, if supported. */

        /* These are our own mode values that map to concrete values at runtime. */
        CONSOLE_MODE_KEEP = CONSOLE_MODE_MAX + 1U,
        CONSOLE_MODE_NEXT,
        CONSOLE_MODE_AUTO,
        CONSOLE_MODE_AUTO_MAX,
};

EFI_STATUS console_key_read(UINT64 *key, UINT64 timeout_usec);
EFI_STATUS console_set_mode(UINT32 mode);
EFI_STATUS console_query_mode(UINTN *x_max, UINTN *y_max);
