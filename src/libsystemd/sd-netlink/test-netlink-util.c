/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <net/if.h>
#include <unistd.h>

#include "sd-netlink.h"

#include "netlink-util.h"
#include "strv.h"
#include "tests.h"

static void test_rtnl_set_link_name(sd_netlink *rtnl) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *m = NULL;
        _cleanup_strv_free_ char **alternative_names = NULL;
        int ifindex, r;

        log_debug("/* %s */", __func__);

        if (geteuid() != 0)
                return (void) log_tests_skipped("not root");

        r = load_module("dummy");
        if (r < 0)
                return (void) log_tests_skipped_errno(r, "Failed to load module 'dummy': %m");

        /* Create a dummy interface to test with. */
        assert_se(sd_rtnl_message_new_link(rtnl, &m, RTM_NEWLINK, 0) >= 0);
        assert_se(m);
        assert_se(sd_netlink_message_append_string(m, IFLA_IFNAME, "test") >= 0);
        assert_se(sd_netlink_message_open_container(m, IFLA_LINKINFO) >= 0);
        assert_se(sd_netlink_message_append_string(m, IFLA_INFO_KIND, "dummy") >= 0);
        assert_se(sd_netlink_message_close_container(m) >= 0);
        assert_se(sd_netlink_call(rtnl, m, -1, NULL) >= 0);
        assert_se((ifindex = if_nametoindex("test")) > 0);

        /* Test that the new name (which is currently an alternative name) is
         * restored as an alternative name on error. Create an error by using
         * an invalid device name, namely one that exceeds IFNAMSIZ
         * (alternative names can exceed IFNAMSIZ, but not regular names). */
        assert_se(rtnl_set_link_alternative_names(&rtnl, ifindex, STRV_MAKE("testlonginterfacename")) >= 0);
        assert_se(rtnl_set_link_name(&rtnl, ifindex, "testlonginterfacename") == -EINVAL);
        assert_se(rtnl_get_link_alternative_names(&rtnl, ifindex, &alternative_names) >= 0);
        assert_se(strv_contains(alternative_names, "testlonginterfacename"));

        /* Cleanup the dummy test interface. */
        assert_se((m = sd_netlink_message_unref(m)) == NULL);
        assert_se(sd_rtnl_message_new_link(rtnl, &m, RTM_DELLINK, ifindex) >= 0);
        assert_se(m);
        assert_se(sd_netlink_call(rtnl, m, -1, NULL) >= 0);
}

int main(void) {
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;

        test_setup_logging(LOG_DEBUG);

        assert_se(sd_netlink_open(&rtnl) >= 0);
        assert_se(rtnl);

        test_rtnl_set_link_name(rtnl);

        return EXIT_SUCCESS;
}
