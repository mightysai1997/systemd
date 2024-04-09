/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <pthread.h>

#include "mempool.h"
#include "process-util.h"
#include "set.h"
#include "tests.h"

#define NUM 100

static void* thread(void *p) {
        Set **s = p;

        ASSERT_TRUE(s);
        assert_se(*s);

        ASSERT_FALSE(is_main_thread());
        ASSERT_TRUE(mempool_enabled);
        ASSERT_FALSE(mempool_enabled());

        assert_se(set_size(*s) == NUM);
        *s = set_free(*s);

        return NULL;
}

static void test_one(const char *val) {
        pthread_t t;
        int x[NUM] = {};
        unsigned i;
        Set *s;

        log_info("Testing with SYSTEMD_MEMPOOL=%s", val);
        ASSERT_EQ(setenv("SYSTEMD_MEMPOOL", val, true), 0);

        ASSERT_TRUE(is_main_thread());
        ASSERT_TRUE(mempool_enabled);    /* It is a weak symbol, but we expect it to be available */
        ASSERT_FALSE(mempool_enabled());

        assert_se(s = set_new(NULL));
        for (i = 0; i < NUM; i++)
                assert_se(set_put(s, &x[i]));

        assert_se(pthread_create(&t, NULL, thread, &s) == 0);
        ASSERT_EQ(pthread_join(t, NULL), 0);

        ASSERT_FALSE(s);
}

TEST(disable_mempool) {
        test_one("0");
        /* The value $SYSTEMD_MEMPOOL= is cached. So the following
         * test should also succeed. */
        test_one("1");
}

DEFINE_TEST_MAIN(LOG_DEBUG);
