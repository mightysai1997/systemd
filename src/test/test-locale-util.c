/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd

  Copyright 2014 Ronny Chevalier

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/


#include "locale-util.h"
#include "macro.h"
#include "strv.h"

static void test_get_locales(void) {
        _cleanup_strv_free_ char **locales = NULL;
        char **p;
        int r;

        r = get_locales(&locales);
        assert_se(r >= 0);
        assert_se(locales);

        STRV_FOREACH(p, locales) {
                puts(*p);
                assert_se(locale_is_valid(*p));
        }
}

static void test_locale_is_valid(void) {
        assert_se(locale_is_valid("en_EN.utf8"));
        assert_se(locale_is_valid("fr_FR.utf8"));
        assert_se(locale_is_valid("fr_FR@euro"));
        assert_se(locale_is_valid("fi_FI"));
        assert_se(locale_is_valid("POSIX"));
        assert_se(locale_is_valid("C"));

        assert_se(!locale_is_valid(""));
        assert_se(!locale_is_valid("/usr/bin/foo"));
        assert_se(!locale_is_valid("\x01gar\x02 bage\x03"));
}

static void test_keymaps(void) {
        _cleanup_strv_free_ char **kmaps = NULL;
        char **p;
        int r;

        assert_se(!keymap_is_valid(""));
        assert_se(!keymap_is_valid("/usr/bin/foo"));
        assert_se(!keymap_is_valid("\x01gar\x02 bage\x03"));

        r = get_keymaps(&kmaps);
        if (r == -ENOENT)
                return; /* skip test if no keymaps are installed */

        assert_se(r >= 0);
        assert_se(kmaps);

        STRV_FOREACH(p, kmaps) {
                puts(*p);
                assert_se(keymap_is_valid(*p));
        }

        assert_se(keymap_is_valid("uk"));
        assert_se(keymap_is_valid("de-nodeadkeys"));
        assert_se(keymap_is_valid("ANSI-dvorak"));
        assert_se(keymap_is_valid("unicode"));
}

static void test_fonts(void) {
        _cleanup_strv_free_ char **fonts = NULL;
        char **p;
        int r;

        assert_se(!font_is_valid(""));
        assert_se(!font_is_valid("/usr/bin/foo"));
        assert_se(!font_is_valid("\x01gar\x02 bage\x03"));

        r = get_kbd_fonts(&fonts);
        if (r == -ENOENT)
                return; /* skip test if no fonts are installed */

        assert_se(r >= 0);
        assert_se(fonts);

        STRV_FOREACH(p, fonts) {
                puts(*p);
                assert_se(font_is_valid(*p));
        }

        assert_se(font_is_valid("eurlatgr"));
}


int main(int argc, char *argv[]) {
        test_get_locales();
        test_locale_is_valid();

        test_keymaps();

        test_fonts();

        return 0;
}
