/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <string.h>

#include "alloc-util.h"
#include "capability-util.h"
#include "cap-list.h"
#include "extract-word.h"
#include "macro.h"
#include "parse-util.h"
#include "stdio-util.h"
#include "string-util.h"

static const struct capability_name* lookup_capability(register const char *str, register GPERF_LEN_TYPE len);

#include "cap-from-name.h"
#include "cap-to-name.h"

const char *capability_to_name(int id) {
        if (id < 0)
                return NULL;
        if (id >= capability_list_length())
                return NULL;

        return capability_names[id];
}

const char *capability_to_string(int id, char buf[static CAPABILITY_TO_STRING_MAX]) {
        const char *p;

        if (id < 0)
                return NULL;
        if (id >= 63) /* refuse caps >= 63 since we can't store them in a uint64_t mask anymore, and still retain UINT64_MAX as marker for "unset" */
                return NULL;

        p = capability_to_name(id);
        if (p)
                return p;

        sprintf(buf, "0x%x", (unsigned) id); /* numerical fallback */
        return buf;
}

int capability_from_name(const char *name) {
        const struct capability_name *sc;
        int r, i;

        assert(name);

        /* Try to parse numeric capability */
        r = safe_atoi(name, &i);
        if (r >= 0) {
                if (i < 0 || i >= 63)
                        return -EINVAL;

                return i;
        }

        /* Try to parse string capability */
        sc = lookup_capability(name, strlen(name));
        if (!sc)
                return -EINVAL;

        return sc->id;
}

/* This is the number of capability names we are *compiled* with.  For the max capability number of the
 * currently-running kernel, use cap_last_cap(). Note that this one returns the size of the array, i.e. one
 * value larger than the last known capability. This is different from cap_last_cap() which returns the
 * highest supported capability. Hence with everyone agreeing on the same capabilities list, this function
 * will return one higher than cap_last_cap(). */
int capability_list_length(void) {
        return (int) MIN(ELEMENTSOF(capability_names), 63U);
}

int capability_set_to_string(uint64_t set, char **ret) {
        _cleanup_free_ char *str = NULL;

        assert(ret);

        for (unsigned i = 0; i <= cap_last_cap(); i++) {
                const char *p;

                if (!(set & (UINT64_C(1) << i)))
                        continue;

                p = CAPABILITY_TO_STRING(i);
                assert(p);

                if (!strextend_with_separator(&str, " ", p))
                        return -ENOMEM;
        }

        if (!str) {
                str = new0(char, 1);
                if (!str)
                        return -ENOMEM;
        }

        *ret = TAKE_PTR(str);
        return 0;
}

int capability_set_from_string(const char *s, uint64_t *ret) {
        uint64_t val = 0;
        bool good = true;

        for (const char *p = s;;) {
                _cleanup_free_ char *word = NULL;
                int r;

                r = extract_first_word(&p, &word, NULL, EXTRACT_UNQUOTE|EXTRACT_RELAX);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                r = capability_from_name(word);
                if (r < 0) {
                        log_debug_errno(r, "Failed to parse capability '%s', ignoring: %m", word);
                        good = false;
                } else
                        val |= UINT64_C(1) << r;
        }

        if (ret)
                *ret = val;

        return good;
}
