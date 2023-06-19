/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once
#include <stdio.h>
#include <errno.h>

#if HAVE_QRENCODE
int dlopen_qrencode(void);

int print_positioned_qrcode(FILE *out, const char *header, const char *string, unsigned row, unsigned column, unsigned tty_width, unsigned tty_height);
int print_qrcode(FILE *out, const char *header, const char *string);
#else
static inline int print_qrcode(FILE *out, const char *header, const char *string) {
        return -EOPNOTSUPP;
}

static inline int print_positioned_qrcode(FILE *out, const char *header, const char *string, unsigned row, unsigned column, unsigned tty_width, unsigned tty_height) {
        return -EOPNOTSUPP;
}
#endif
