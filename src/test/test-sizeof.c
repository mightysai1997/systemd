/* SPDX-License-Identifier: LGPL-2.1+ */

#include <stdio.h>
#include <string.h>

#include "time-util.h"

/* Print information about various types. Useful when diagnosing
 * gcc diagnostics on an unfamiliar architecture. */

#pragma GCC diagnostic ignored "-Wtype-limits"

#define info(t)                                                 \
        printf("%s → %zu bits%s\n", STRINGIFY(t),               \
               sizeof(t)*CHAR_BIT,                              \
               strstr(STRINGIFY(t), "signed") ? "" :            \
               ((t)-1 < (t)0 ? ", signed" : ", unsigned"));

enum Enum {
        enum_value,
};

enum BigEnum {
        big_enum_value = UINT64_C(1),
};

enum BigEnum2 {
        big_enum2_pos = UINT64_C(1),
        big_enum2_neg = UINT64_C(-1),
};

int main(void) {
        info(char);
        info(signed char);
        info(unsigned char);
        info(short unsigned);
        info(unsigned);
        info(long unsigned);
        info(long long unsigned);
        info(__syscall_ulong_t);
        info(__syscall_slong_t);

        info(float);
        info(double);
        info(long double);

#if defined(__HAVE_DISTINCT_FLOAT128X) && __HAVE_DISTINCT_FLOAT128X
        info(_Float128x);
#endif
#if defined(__HAVE_DISTINCT_FLOAT128) && __HAVE_DISTINCT_FLOAT128
        info(_Float128);
#endif
#if defined(__HAVE_DISTINCT_FLOAT64X) && __HAVE_DISTINCT_FLOAT64X
        info(_Float64x);
#endif
#if defined(__HAVE_DISTINCT_FLOAT64) && __HAVE_DISTINCT_FLOAT64
        info(_Float64);
#endif
#if defined(__HAVE_DISTINCT_FLOAT32X) && __HAVE_DISTINCT_FLOAT32X
        info(_Float32x);
#endif
#if defined(__HAVE_DISTINCT_FLOAT32) && __HAVE_DISTINCT_FLOAT32
        info(_Float32);
#endif

        info(size_t);
        info(ssize_t);
        info(time_t);
        info(usec_t);
        info(__time_t);
        info(pid_t);
        info(uid_t);
        info(gid_t);

        info(enum Enum);
        info(enum BigEnum);
        info(enum BigEnum2);
        assert_cc(sizeof(enum BigEnum2) == 8);
        printf("big_enum2_pos → %zu\n", sizeof(big_enum2_pos));
        printf("big_enum2_neg → %zu\n", sizeof(big_enum2_neg));

        return 0;
}
