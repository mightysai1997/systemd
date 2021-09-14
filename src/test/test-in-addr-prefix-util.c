/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "in-addr-prefix-util.h"
#include "tests.h"

static void test_config_parse_in_addr_prefixes_one(int family, const union in_addr_union *addr, uint8_t prefixlen, Set **prefixes) {
        _cleanup_free_ char *str = NULL;

        assert_se(in_addr_prefix_to_string(family, addr, prefixlen, &str) >= 0);
        assert_se(config_parse_in_addr_prefixes("unit", "filename", 1, "Service", 1, "IPAddressAllow", 0, str, prefixes, NULL) >= 0);
}

static void test_config_parse_in_addr_prefixes(Set **ret) {
        _cleanup_set_free_ Set *prefixes = NULL;

        log_info("/* %s() */", __func__);

        for (uint32_t i = 0; i < 256; i++) {
                /* ipv4 link-local address */
                test_config_parse_in_addr_prefixes_one(AF_INET, &(union in_addr_union) {
                                .in.s_addr = htobe32((UINT32_C(169) << 24) |
                                                     (UINT32_C(254) << 16) |
                                                     (i << 8)),
                        }, 24, &prefixes);

                /* ipv6 multicast address */
                test_config_parse_in_addr_prefixes_one(AF_INET6, &(union in_addr_union) {
                                .in6.s6_addr[0] = 0xff,
                                .in6.s6_addr[1] = i,
                        }, 16, &prefixes);

                for (uint32_t j = 0; j < 256; j++) {
                        test_config_parse_in_addr_prefixes_one(AF_INET, &(union in_addr_union) {
                                        .in.s_addr = htobe32((UINT32_C(169) << 24) |
                                                             (UINT32_C(254) << 16) |
                                                             (i << 8) | j),
                                }, 32, &prefixes);

                        test_config_parse_in_addr_prefixes_one(AF_INET6, &(union in_addr_union) {
                                        .in6.s6_addr[0] = 0xff,
                                        .in6.s6_addr[1] = i,
                                        .in6.s6_addr[2] = j,
                                }, 24, &prefixes);
                }
        }

        *ret = TAKE_PTR(prefixes);
}

static void test_in_addr_prefixes_reduce(Set *prefixes) {
        log_info("/* %s() */", __func__);

        assert_se(set_size(prefixes) == 2 * 256 * 257);
        assert_se(!in_addr_prefixes_is_any(prefixes));

        assert_se(in_addr_prefixes_reduce(prefixes) >= 0);
        assert_se(set_size(prefixes) == 2 * 256);
        assert_se(!in_addr_prefixes_is_any(prefixes));

        assert_se(config_parse_in_addr_prefixes("unit", "filename", 1, "Service", 1, "IPAddressAllow", 0, "link-local", &prefixes, NULL) == 0);
        assert_se(set_size(prefixes) == 2 * 256 + 2);
        assert_se(!in_addr_prefixes_is_any(prefixes));

        assert_se(in_addr_prefixes_reduce(prefixes) >= 0);
        assert_se(set_size(prefixes) == 256 + 2);
        assert_se(!in_addr_prefixes_is_any(prefixes));

        assert_se(config_parse_in_addr_prefixes("unit", "filename", 1, "Service", 1, "IPAddressAllow", 0, "multicast", &prefixes, NULL) == 0);
        assert_se(set_size(prefixes) == 256 + 4);
        assert_se(!in_addr_prefixes_is_any(prefixes));

        assert_se(in_addr_prefixes_reduce(prefixes) >= 0);
        assert_se(set_size(prefixes) == 4);
        assert_se(!in_addr_prefixes_is_any(prefixes));

        assert_se(config_parse_in_addr_prefixes("unit", "filename", 1, "Service", 1, "IPAddressAllow", 0, "any", &prefixes, NULL) == 0);
        assert_se(set_size(prefixes) == 6);
        assert_se(in_addr_prefixes_is_any(prefixes));

        assert_se(in_addr_prefixes_reduce(prefixes) >= 0);
        assert_se(set_size(prefixes) == 2);
        assert_se(in_addr_prefixes_is_any(prefixes));
}

int main(int argc, char *argv[]) {
        _cleanup_set_free_ Set *prefixes = NULL;

        test_setup_logging(LOG_DEBUG);

        test_config_parse_in_addr_prefixes(&prefixes);
        test_in_addr_prefixes_reduce(prefixes);

        return 0;
}
