/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>

#include "securebits-util.h"
#include "strv.h"
#include "tests.h"
#include "unit-file.h"

static const char * const string_bits[] = {
        "keep-caps",
        "keep-caps-locked",
        "no-setuid-fixup",
        "no-setuid-fixup-locked",
        "noroot",
        "noroot-locked",
        NULL
};

TEST(secure_bits_basic) {
        _cleanup_free_ char *joined = NULL, *str = NULL;
        int r;

        /* Check if converting each bit from string and back to string yields
         * the same value */
        STRV_FOREACH(bit, string_bits) {
                _cleanup_free_ char *s = NULL;

                r = secure_bits_from_string(*bit);
                ASSERT_GT(r, 0);
                ASSERT_TRUE(secure_bits_is_valid(r));
                assert_se(secure_bits_to_string_alloc(r, &s) >= 0);
                printf("%s = 0x%x = %s\n", *bit, (unsigned)r, s);
                assert_se(streq(*bit, s));
        }

        /* Ditto, but with all bits at once */
        joined = strv_join((char**)string_bits, " ");
        ASSERT_TRUE(joined);
        r = secure_bits_from_string(joined);
        ASSERT_GT(r, 0);
        ASSERT_TRUE(secure_bits_is_valid(r));
        assert_se(secure_bits_to_string_alloc(r, &str) >= 0);
        printf("%s = 0x%x = %s\n", joined, (unsigned)r, str);
        assert_se(streq(joined, str));

        str = mfree(str);

        /* Empty string */
        ASSERT_EQ(secure_bits_from_string(""), 0);
        assert_se(secure_bits_from_string("     ") == 0);

        /* Only invalid entries */
        assert_se(secure_bits_from_string("foo bar baz") == 0);

        /* Empty secure bits */
        assert_se(secure_bits_to_string_alloc(0, &str) >= 0);
        ASSERT_TRUE(isempty(str));

        str = mfree(str);

        /* Bits to string with check */
        assert_se(secure_bits_to_string_alloc_with_check(INT_MAX, &str) == -EINVAL);
        ASSERT_NULL(str);
        assert_se(secure_bits_to_string_alloc_with_check(
                                (1 << SECURE_KEEP_CAPS) | (1 << SECURE_KEEP_CAPS_LOCKED),
                                &str) >= 0);
        assert_se(streq(str, "keep-caps keep-caps-locked"));
}

TEST(secure_bits_mix) {
        static struct sbit_table {
                const char *input;
                const char *expected;
        } sbit_table[] = {
                { "keep-caps keep-caps keep-caps",  "keep-caps" },
                { "keep-caps noroot keep-caps",     "keep-caps noroot" },
                { "noroot foo bar baz noroot",      "noroot" },
                { "noroot \"foo\" \"bar keep-caps", "noroot" },
                { "\"noroot foo\" bar keep-caps",   "keep-caps" },
                {}
        };

        for (const struct sbit_table *s = sbit_table; s->input; s++) {
                _cleanup_free_ char *str = NULL;
                int r;

                r = secure_bits_from_string(s->input);
                ASSERT_GT(r, 0);
                ASSERT_TRUE(secure_bits_is_valid(r));
                assert_se(secure_bits_to_string_alloc(r, &str) >= 0);
                printf("%s = 0x%x = %s\n", s->input, (unsigned)r, str);
                assert_se(streq(s->expected, str));
        }
}

DEFINE_TEST_MAIN(LOG_DEBUG);
