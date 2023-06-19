/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "qrcode-util.h"

#if HAVE_QRENCODE
#include <qrencode.h>

#include "dlfcn-util.h"
#include "locale-util.h"
#include "log.h"
#include "strv.h"
#include "terminal-util.h"

#define ANSI_WHITE_ON_BLACK "\033[40;37;1m"
#define UNICODE_FULL_BLOCK       u8"█"
#define UNICODE_LOWER_HALF_BLOCK u8"▄"
#define UNICODE_UPPER_HALF_BLOCK u8"▀"

static void *qrcode_dl = NULL;

static QRcode* (*sym_QRcode_encodeString)(const char *string, int version, QRecLevel level, QRencodeMode hint, int casesensitive) = NULL;
static void (*sym_QRcode_free)(QRcode *qrcode) = NULL;

int dlopen_qrencode(void) {
        int r;

        FOREACH_STRING(s, "libqrencode.so.4", "libqrencode.so.3") {
                r = dlopen_many_sym_or_warn(
                        &qrcode_dl, s, LOG_DEBUG,
                        DLSYM_ARG(QRcode_encodeString),
                        DLSYM_ARG(QRcode_free));
                if (r >= 0)
                        break;
        }

        return r;
}

static void print_positioned_border(FILE *output, unsigned width, unsigned row, unsigned column) {
        /* Four rows of border */
        int fd = fileno(output);
        if (fd < 0)
                return (void)log_warning_errno(fd, "unable to get file descriptor from the file stream: %m");

        set_terminal_cursor_position(fd, row, column);
        for (unsigned y = 0; y < 4; y += 2) {
                fputs(ANSI_WHITE_ON_BLACK, output);

                for (unsigned x = 0; x < 4 + width + 4; x++)
                        fputs(UNICODE_FULL_BLOCK, output);

                fputs(ANSI_NORMAL "\n", output);
                set_terminal_cursor_position(fd, row + 1, column);
        }
}

static void write_positioned_qrcode(FILE *output, QRcode *qr, unsigned int row, unsigned int column) {
        assert(qr);

        if (!output)
                output = stdout;

        int fd, move_down = 3;

        fd = fileno(output);
        if (fd < 0)
                return (void)log_warning_errno(fd, "unable to get file descriptor from the file stream: %m");

        print_positioned_border(output, qr->width, row, column);

        set_terminal_cursor_position(fd, row + 2, column);
        for (unsigned y = 0; y < (unsigned) qr->width; y += 2) {
                const uint8_t *row1 = qr->data + qr->width * y;
                const uint8_t *row2 = row1 + qr->width;

                fputs(ANSI_WHITE_ON_BLACK, output);

                for (unsigned x = 0; x < 4; x++)
                        fputs(UNICODE_FULL_BLOCK, output);

                for (unsigned x = 0; x < (unsigned) qr->width; x++) {
                        bool a, b;

                        a = row1[x] & 1;
                        b = (y+1) < (unsigned) qr->width ? (row2[x] & 1) : false;

                        if (a && b)
                                fputc(' ', output);
                        else if (a)
                                fputs(UNICODE_LOWER_HALF_BLOCK, output);
                        else if (b)
                                fputs(UNICODE_UPPER_HALF_BLOCK, output);
                        else
                                fputs(UNICODE_FULL_BLOCK, output);
                }

                for (unsigned x = 0; x < 4; x++)
                        fputs(UNICODE_FULL_BLOCK, output);
                set_terminal_cursor_position(fd, row + move_down, column);
                move_down += 1;
                fputs(ANSI_NORMAL "\n", output);
        }

        print_positioned_border(output, qr->width, row + move_down, column);
        fflush(output);
}

int print_positioned_qrcode(FILE *out, const char *header, const char *string, unsigned int row, unsigned int column) {
        QRcode* qr;
        int r, fd;

        fd = fileno(out);
        if (fd < 0)
                return log_warning_errno(fd, "unable to get file descriptor from the file stream: %m");

        /* If this is not an UTF-8 system or ANSI colors aren't supported/disabled don't print any QR
         * codes */
        if (!is_locale_utf8() || !colors_enabled())
                return -EOPNOTSUPP;

        r = dlopen_qrencode();
        if (r < 0)
                return r;

        qr = sym_QRcode_encodeString(string, 0, QR_ECLEVEL_L, QR_MODE_8, 1);
        if (!qr)
                return -ENOMEM;

        if (header) {
                set_terminal_cursor_position(fd, row - 1, column);
                fprintf(out, "\n%s:\n\n", header);
        }

        write_positioned_qrcode(out, qr, row, column);

        fputc('\n', out);

        sym_QRcode_free(qr);
        return 0;
}
#endif
