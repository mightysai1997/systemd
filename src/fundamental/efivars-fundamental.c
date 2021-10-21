/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "efivars-fundamental.h"

static const sd_char * const table[_SECURE_BOOT_MAX] = {
        [SECURE_BOOT_UNSUPPORTED] = STR_C("unsupported"),
        [SECURE_BOOT_UNKNOWN]     = STR_C("unknown"),
        [SECURE_BOOT_AUDIT]       = STR_C("audit"),
        [SECURE_BOOT_DEPLOYED]    = STR_C("deployed"),
        [SECURE_BOOT_SETUP]       = STR_C("setup"),
        [SECURE_BOOT_USER]        = STR_C("user"),
};

const sd_char *secure_boot_mode_to_string(SecureBootMode m) {
        return (m >= 0 && m < _SECURE_BOOT_MAX) ? table[m] : NULL;
}

SecureBootMode decode_secure_boot_mode(
                sd_bool secure,
                sd_bool audit,
                sd_bool deployed,
                sd_bool setup) {

        /* See figure 32-4 Secure Boot Modes from UEFI Specification 2.9 */
        if (secure && deployed && !audit && !setup)
                return SECURE_BOOT_DEPLOYED;
        if (secure && !deployed && !audit && !setup)
                return SECURE_BOOT_USER;
        if (!secure && !deployed && audit && setup)
                return SECURE_BOOT_AUDIT;
        if (!secure && !deployed && !audit && setup)
                return SECURE_BOOT_SETUP;

        /* Well, this should not happen. */
        return SECURE_BOOT_UNKNOWN;
}
