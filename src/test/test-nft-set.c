/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <assert.h>
#include <unistd.h>

#include "firewall-util.h"
#include "in-addr-util.h"
#include "log.h"
#include "parse-util.h"
#include "string-util.h"
#include "tests.h"

int main(int argc, char **argv) {
        int r;

        assert_se(argc == 7);

        test_setup_logging(LOG_DEBUG);

        if (getuid() != 0)
                return log_tests_skipped("not root");

        int nfproto;
        nfproto = nfproto_from_string(argv[2]);
        assert_se(nfproto > 0);

        FirewallContext *ctx;
        r = fw_ctx_new(&ctx, /* init_tables = */ false);
        assert_se(r == 0);

        const NFTSetContext nft_set_context = {
                .nfproto = nfproto,
                .table = argv[3],
                .set = argv[4],
        };

        bool add;
        if (streq(argv[1], "add"))
                add = true;
        else
                add = false;

        if (streq(argv[5], "in_addr")) {
                union in_addr_union addr;
                int af;
                unsigned char prefixlen;

                r = in_addr_prefix_from_string_auto(argv[6], &af, &addr, &prefixlen);
                assert_se(r == 0);

                r = nft_set_element_op_in_addr_open(&ctx, add, &nft_set_context, af, &addr, prefixlen);
                assert_se(r == 0);
        }

        return 0;
}
