/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "bootspec.h"
#include "fileio.h"
#include "path-util.h"
#include "rm-rf.h"
#include "tests.h"
#include "tmpfile-util.h"

TEST_RET(bootspec_sort) {

        static const struct {
                const char *fname;
                const char *contents;
        } entries[] = {
                {
                        .fname = "a-10.conf",
                        .contents =
                        "title A\n"
                        "version 10\n"
                        "machine-id dd235d00696545768f6f693bfd23b15f\n",
                },
                {
                        .fname = "a-5.conf",
                        .contents =
                        "title A\n"
                        "version 5\n"
                        "machine-id dd235d00696545768f6f693bfd23b15f\n",
                },
                {
                        .fname = "b.conf",
                        .contents =
                        "title B\n"
                        "version 3\n"
                        "machine-id b75451ad92f94feeab50b0b442768dbd\n",
                },
                {
                        .fname = "c.conf",
                        .contents =
                        "title C\n"
                        "sort-key xxxx\n"
                        "version 5\n"
                        "machine-id 309de666fd5044268a9a26541ac93176\n",
                },
                {
                        .fname = "cx.conf",
                        .contents =
                        "title C\n"
                        "sort-key xxxx\n"
                        "version 10\n"
                        "machine-id 309de666fd5044268a9a26541ac93176\n",
                },
                {
                        .fname = "d.conf",
                        .contents =
                        "title D\n"
                        "sort-key kkkk\n"
                        "version 100\n"
                        "machine-id 81c6e3147cf544c19006af023e22b292\n",
                },
        };

        _cleanup_(rm_rf_physical_and_freep) char *d = NULL;
        _cleanup_(boot_config_free) BootConfig config = BOOT_CONFIG_NULL;

        assert_se(mkdtemp_malloc("/tmp/bootspec-testXXXXXX", &d) >= 0);

        for (size_t i = 0; i < ELEMENTSOF(entries); i++) {
                _cleanup_free_ char *j = NULL;

                j = path_join(d, "/loader/entries/", entries[i].fname);
                assert_se(j);

                assert_se(write_string_file(j, entries[i].contents, WRITE_STRING_FILE_CREATE|WRITE_STRING_FILE_MKDIR_0755) >= 0);
        }

        assert_se(boot_config_load(&config, d, NULL) >= 0);

        assert_se(config.n_entries == 6);

        /* First, because has sort key, and its the lowest one */
        assert_se(streq(config.entries[0].id, "d.conf"));

        /* These two have a sort key, and newest must be first */
        assert_se(streq(config.entries[1].id, "cx.conf"));
        assert_se(streq(config.entries[2].id, "c.conf"));

        /* The following ones have no sort key, hence order by version compared ids, lowest first */
        assert_se(streq(config.entries[3].id, "b.conf"));
        assert_se(streq(config.entries[4].id, "a-10.conf"));
        assert_se(streq(config.entries[5].id, "a-5.conf"));

        return 0;
}

static void test_extract_tries_one(const char *fname, int ret, const char *stripped, unsigned tries_left, unsigned tries_done) {
        _cleanup_free_ char *p = NULL;
        unsigned l = UINT_MAX, d = UINT_MAX;

        assert_se(boot_filename_extract_tries(fname, &p, &l, &d) == ret);
        assert_se(streq_ptr(p, stripped));
        assert_se(l == tries_left);
        assert_se(d == tries_done);
}

TEST_RET(bootspec_extract_tries) {
        test_extract_tries_one("foo.conf", 0, "foo.conf", UINT_MAX, UINT_MAX);

        test_extract_tries_one("foo+0.conf", 0, "foo.conf", 0, UINT_MAX);
        test_extract_tries_one("foo+1.conf", 0, "foo.conf", 1, UINT_MAX);
        test_extract_tries_one("foo+2.conf", 0, "foo.conf", 2, UINT_MAX);
        test_extract_tries_one("foo+33.conf", 0, "foo.conf", 33, UINT_MAX);

        assert_cc(UINT_MAX == UINT32_MAX);
        test_extract_tries_one("foo+4294967294.conf", 0, "foo.conf", 4294967294, UINT_MAX);
        test_extract_tries_one("foo+4294967295.conf", -ERANGE, NULL, UINT_MAX, UINT_MAX);
        test_extract_tries_one("foo+4294967296.conf", -ERANGE, NULL, UINT_MAX, UINT_MAX);

        test_extract_tries_one("foo+33-0.conf", 0, "foo.conf", 33, 0);
        test_extract_tries_one("foo+33-1.conf", 0, "foo.conf", 33, 1);
        test_extract_tries_one("foo+33-107.conf", 0, "foo.conf", 33, 107);
        test_extract_tries_one("foo+33-107.efi", 0, "foo.efi", 33, 107);
        test_extract_tries_one("foo+33-4294967294.conf", 0, "foo.conf", 33, 4294967294);
        test_extract_tries_one("foo+33-4294967295.conf", -ERANGE, NULL, UINT_MAX, UINT_MAX);
        test_extract_tries_one("foo+33-4294967296.conf", -ERANGE, NULL, UINT_MAX, UINT_MAX);

        test_extract_tries_one("foo+007-000008.conf", -EINVAL, NULL, UINT_MAX, UINT_MAX);

        test_extract_tries_one("foo-1.conf", 0, "foo-1.conf", UINT_MAX, UINT_MAX);
        test_extract_tries_one("foo-999.conf", 0, "foo-999.conf", UINT_MAX, UINT_MAX);
        test_extract_tries_one("foo-.conf", 0, "foo-.conf", UINT_MAX, UINT_MAX);

        test_extract_tries_one("foo+.conf", 0, "foo+.conf", UINT_MAX, UINT_MAX);
        test_extract_tries_one("+.conf", 0, "+.conf", UINT_MAX, UINT_MAX);
        test_extract_tries_one("-.conf", 0, "-.conf", UINT_MAX, UINT_MAX);
        test_extract_tries_one("", 0, "", UINT_MAX, UINT_MAX);

        test_extract_tries_one("+1.", 0, ".", 1, UINT_MAX);
        test_extract_tries_one("+1-7.", 0, ".", 1, 7);

        return 0;
}

DEFINE_TEST_MAIN(LOG_INFO);
