/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdio.h>

#include "fdset.h"
#include "image-policy.h"
#include "macro.h"
#include "set.h"
#include "string-util.h"
#include "time-util.h"

int serialize_item(FILE *f, const char *key, const char *value);
int serialize_item_escaped(FILE *f, const char *key, const char *value);
int serialize_item_format(FILE *f, const char *key, const char *value, ...) _printf_(3,4);
int serialize_item_hexmem(FILE *f, const char *key, const void *p, size_t l);
int serialize_item_base64mem(FILE *f, const char *key, const void *p, size_t l);
int serialize_fd_full(FILE *f, FDSet *fds, bool store_index, const char *key, int fd);
static inline int serialize_fd(FILE *f, FDSet *fds, const char *key, int fd) {
        return serialize_fd_full(f, fds, /* store_index= */ false, key, fd);
}
int serialize_usec(FILE *f, const char *key, usec_t usec);
int serialize_dual_timestamp(FILE *f, const char *key, const dual_timestamp *t);
int serialize_strv(FILE *f, const char *key, char **l);
int serialize_string_set(FILE *f, const char *key, Set *s);
int serialize_image_policy(FILE *f, const char *key, const ImagePolicy *p);

static inline int serialize_bool(FILE *f, const char *key, bool b) {
        return serialize_item(f, key, yes_no(b));
}
static inline int serialize_bool_elide(FILE *f, const char *key, bool b) {
        return b ? serialize_item(f, key, yes_no(b)) : 0;
}

#define serialize_item_format_tristate(file, field, value) \
        serialize_item_format(file, field, "%i", value)

int deserialize_read_line(FILE *f, char **ret);

int deserialize_usec(const char *value, usec_t *timestamp);
int deserialize_dual_timestamp(const char *value, dual_timestamp *t);
int deserialize_environment(const char *value, char ***environment);
int deserialize_strv(char ***l, const char *value);
int deserialize_fd_set(const char *value, FDSet *fds);
int deserialize_fd_array(const char *value, int *fds_array, size_t n_fds_array);

int open_serialization_fd(const char *ident);
int open_serialization_file(const char *ident, FILE **ret);
