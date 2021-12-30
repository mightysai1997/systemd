/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <string.h>

#include "alloc-util.h"
#include "fuzz.h"
#include "udev-util.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
        _cleanup_free_ char *str = NULL;
        int r;
        char *value = UINT_TO_PTR(0x12345678U);
        char *endpos = UINT_TO_PTR(0x87654321U);

        str = memdup_suffix0(data, size);
        assert_se(str);

        r = udev_rule_parse_value(str, &value, &endpos);
        if (r < 0) {
                /* not modified on failure */
                assert_se(value == UINT_TO_PTR(0x12345678U));
                assert_se(endpos == UINT_TO_PTR(0x87654321U));
        } else {
                assert_se(endpos <= str + size);
                assert_se(endpos > str + 1);
        }

        return 0;
}
