/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>

#include "macro.h"
#include "path-util.h"

bool dirent_is_file(const struct dirent *de) _pure_;
bool dirent_is_file_with_suffix(const struct dirent *de, const char *suffix) _pure_;

struct dirent *readdir_ensure_type(DIR *d);
struct dirent *readdir_no_dot(DIR *dirp);

#define FOREACH_DIRENT(de, d, on_error)                                 \
        for (de = readdir_ensure_type(d);; de = readdir_ensure_type(d)) \
                if (!de) {                                              \
                        if (errno > 0) {                                \
                                on_error;                               \
                        }                                               \
                        break;                                          \
                } else if (hidden_or_backup_file((de)->d_name))         \
                        continue;                                       \
                else

#define FOREACH_DIRENT_ALL(de, d, on_error)                             \
        for (de = readdir_ensure_type(d);; de = readdir_ensure_type(d)) \
                if (!de) {                                              \
                        if (errno > 0) {                                \
                                on_error;                               \
                        }                                               \
                        break;                                          \
                } else
