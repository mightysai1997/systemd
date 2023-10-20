/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <sys/types.h>
#include <sys/uio.h>

#include "alloc-util.h"
#include "macro.h"

size_t iovec_total_size(const struct iovec *i, size_t n);

bool iovec_increment(struct iovec *i, size_t n, size_t k);

#define IOVEC_NULL (const struct iovec) {}

#define IOVEC_MAKE(base, len) (struct iovec) { .iov_base = (base), .iov_len = (len) }
#define IOVEC_MAKE_STRING(string)                       \
        ({                                              \
                const char *_s = (string);              \
                IOVEC_MAKE((char*) _s, strlen(_s));     \
        })

#define TAKE_IOVEC(p) TAKE_GENERIC((p), struct iovec, IOVEC_NULL)

static inline void iovec_done(struct iovec *iovec) {
        /* A _cleanup_() helper that frees the iov_base in the iovec */
        assert(iovec);
        free(iovec->iov_base);
}

static inline void iovec_done_erase(struct iovec *iovec) {
        assert(iovec);
        erase_and_free(iovec->iov_base);
}

static inline bool iovec_is_set(const struct iovec *iov) {
        return iov && iov->iov_len > 0 && iov->iov_base;
}

char* set_iovec_string_field(struct iovec *iovec, size_t *n_iovec, const char *field, const char *value);
char* set_iovec_string_field_free(struct iovec *iovec, size_t *n_iovec, const char *field, char *value);

void iovec_array_free(struct iovec *iov, size_t n);
