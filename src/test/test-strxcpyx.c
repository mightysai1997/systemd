/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdio.h>

#include "string-util.h"
#include "strxcpyx.h"
#include "tests.h"

TEST(strpcpy) {
        char target[25];
        char *s = target;
        size_t space_left;
        bool truncated;

        space_left = sizeof(target);
        space_left = strpcpy_full(&s, space_left, "12345", &truncated);
        ASSERT_FALSE(truncated);
        space_left = strpcpy_full(&s, space_left, "hey hey hey", &truncated);
        ASSERT_FALSE(truncated);
        space_left = strpcpy_full(&s, space_left, "waldo", &truncated);
        ASSERT_FALSE(truncated);
        space_left = strpcpy_full(&s, space_left, "ba", &truncated);
        ASSERT_FALSE(truncated);
        space_left = strpcpy_full(&s, space_left, "r", &truncated);
        ASSERT_FALSE(truncated);
        ASSERT_EQ(space_left, 1u);
        assert_se(streq(target, "12345hey hey heywaldobar"));

        space_left = strpcpy_full(&s, space_left, "", &truncated);
        ASSERT_FALSE(truncated);
        ASSERT_EQ(space_left, 1u);
        assert_se(streq(target, "12345hey hey heywaldobar"));

        space_left = strpcpy_full(&s, space_left, "f", &truncated);
        ASSERT_TRUE(truncated);
        ASSERT_EQ(space_left, 0u);
        assert_se(streq(target, "12345hey hey heywaldobar"));

        space_left = strpcpy_full(&s, space_left, "", &truncated);
        ASSERT_FALSE(truncated);
        ASSERT_EQ(space_left, 0u);
        assert_se(streq(target, "12345hey hey heywaldobar"));

        space_left = strpcpy_full(&s, space_left, "foo", &truncated);
        ASSERT_TRUE(truncated);
        ASSERT_EQ(space_left, 0u);
        assert_se(streq(target, "12345hey hey heywaldobar"));
}

TEST(strpcpyf) {
        char target[25];
        char *s = target;
        size_t space_left;
        bool truncated;

        space_left = sizeof(target);
        space_left = strpcpyf_full(&s, space_left, &truncated, "space left: %zu. ", space_left);
        ASSERT_FALSE(truncated);
        space_left = strpcpyf_full(&s, space_left, &truncated, "foo%s", "bar");
        ASSERT_FALSE(truncated);
        ASSERT_EQ(space_left, 3u);
        assert_se(streq(target, "space left: 25. foobar"));

        space_left = strpcpyf_full(&s, space_left, &truncated, "%i", 42);
        ASSERT_FALSE(truncated);
        ASSERT_EQ(space_left, 1u);
        assert_se(streq(target, "space left: 25. foobar42"));

        space_left = strpcpyf_full(&s, space_left, &truncated, "%s", "");
        ASSERT_FALSE(truncated);
        ASSERT_EQ(space_left, 1u);
        assert_se(streq(target, "space left: 25. foobar42"));

        space_left = strpcpyf_full(&s, space_left, &truncated, "%c", 'x');
        ASSERT_TRUE(truncated);
        ASSERT_EQ(space_left, 0u);
        assert_se(streq(target, "space left: 25. foobar42"));

        space_left = strpcpyf_full(&s, space_left, &truncated, "%s", "");
        ASSERT_FALSE(truncated);
        ASSERT_EQ(space_left, 0u);
        assert_se(streq(target, "space left: 25. foobar42"));

        space_left = strpcpyf_full(&s, space_left, &truncated, "abc%s", "hoge");
        ASSERT_TRUE(truncated);
        ASSERT_EQ(space_left, 0u);
        assert_se(streq(target, "space left: 25. foobar42"));

        /* test overflow */
        s = target;
        space_left = strpcpyf_full(&s, 12, &truncated, "00 left: %i. ", 999);
        ASSERT_TRUE(truncated);
        assert_se(streq(target, "00 left: 99"));
        ASSERT_EQ(space_left, 0u);
        assert_se(target[12] == '2');
}

TEST(strpcpyl) {
        char target[25];
        char *s = target;
        size_t space_left;
        bool truncated;

        space_left = sizeof(target);
        space_left = strpcpyl_full(&s, space_left, &truncated, "waldo", " test", " waldo. ", NULL);
        ASSERT_FALSE(truncated);
        space_left = strpcpyl_full(&s, space_left, &truncated, "Banana", NULL);
        ASSERT_FALSE(truncated);
        ASSERT_EQ(space_left, 1u);
        assert_se(streq(target, "waldo test waldo. Banana"));

        space_left = strpcpyl_full(&s, space_left, &truncated, "", "", "", NULL);
        ASSERT_FALSE(truncated);
        ASSERT_EQ(space_left, 1u);
        assert_se(streq(target, "waldo test waldo. Banana"));

        space_left = strpcpyl_full(&s, space_left, &truncated, "", "x", "", NULL);
        ASSERT_TRUE(truncated);
        ASSERT_EQ(space_left, 0u);
        assert_se(streq(target, "waldo test waldo. Banana"));

        space_left = strpcpyl_full(&s, space_left, &truncated, "hoge", NULL);
        ASSERT_TRUE(truncated);
        ASSERT_EQ(space_left, 0u);
        assert_se(streq(target, "waldo test waldo. Banana"));
}

TEST(strscpy) {
        char target[25];
        size_t space_left;
        bool truncated;

        space_left = sizeof(target);
        space_left = strscpy_full(target, space_left, "12345", &truncated);
        ASSERT_FALSE(truncated);

        assert_se(streq(target, "12345"));
        ASSERT_EQ(space_left, 20u);
}

TEST(strscpyl) {
        char target[25];
        size_t space_left;
        bool truncated;

        space_left = sizeof(target);
        space_left = strscpyl_full(target, space_left, &truncated, "12345", "waldo", "waldo", NULL);
        ASSERT_FALSE(truncated);

        assert_se(streq(target, "12345waldowaldo"));
        ASSERT_EQ(space_left, 10u);
}

TEST(sd_event_code_migration) {
        char b[100 * DECIMAL_STR_MAX(unsigned) + 1];
        char c[100 * DECIMAL_STR_MAX(unsigned) + 1], *p;
        unsigned i;
        size_t l;
        int o, r;

        for (i = o = 0; i < 100; i++) {
                r = snprintf(&b[o], sizeof(b) - o, "%u ", i);
                assert_se(r >= 0 && r < (int) sizeof(b) - o);
                o += r;
        }

        p = c;
        l = sizeof(c);
        for (i = 0; i < 100; i++)
                l = strpcpyf(&p, l, "%u ", i);

        assert_se(streq(b, c));
}

DEFINE_TEST_MAIN(LOG_INFO);
