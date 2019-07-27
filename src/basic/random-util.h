/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum RandomFlags {
        RANDOM_EXTEND_WITH_PSEUDO = 1 << 0, /* If we can't get enough genuine randomness, but some, fill up the rest with pseudo-randomness */
        RANDOM_BLOCK              = 1 << 1, /* Rather block than return crap randomness (only if the kernel supports that) */
        RANDOM_MAY_FAIL           = 1 << 2, /* If we can't get any randomness at all, return early with -ENODATA */
        RANDOM_ALLOW_RDRAND       = 1 << 3, /* Allow usage of the CPU RNG */
} RandomFlags;

int genuine_random_bytes(void *p, size_t n, RandomFlags flags); /* returns "genuine" randomness, optionally filled up with pseudo random, if not enough is available */
void pseudo_random_bytes(void *p, size_t n);                    /* returns only pseudo-randommess (but possibly seeded from something better) */
void random_bytes(void *p, size_t n);                           /* returns genuine randomness if cheaply available, and pseudo randomness if not. */

void initialize_srand(void);

static inline uint64_t random_u64(void) {
        uint64_t u;
        random_bytes(&u, sizeof(u));
        return u;
}

static inline uint32_t random_u32(void) {
        uint32_t u;
        random_bytes(&u, sizeof(u));
        return u;
}

int rdrand(unsigned long *ret);

/* Some limits on the pool sizes when we deal with the kernel random pool */
#define RANDOM_POOL_SIZE_MIN 512U
#define RANDOM_POOL_SIZE_MAX (10U*1024U*1024U)

size_t random_pool_size(void);
