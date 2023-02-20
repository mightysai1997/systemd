/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "set.h"

typedef enum CopyFlags {
        COPY_REFLINK       = 1 << 0,  /* Try to reflink */
        COPY_MERGE         = 1 << 1,  /* Merge existing trees with our new one to copy */
        COPY_REPLACE       = 1 << 2,  /* Replace an existing file if there's one */
        COPY_SAME_MOUNT    = 1 << 3,  /* Don't descend recursively into other file systems, across mount point boundaries */
        COPY_MERGE_EMPTY   = 1 << 4,  /* Merge an existing, empty directory with our new tree to copy */
        COPY_CRTIME        = 1 << 5,  /* Generate a user.crtime_usec xattr off the source crtime if there is one, on copying */
        COPY_SIGINT        = 1 << 6,  /* Check for SIGINT regularly and return EINTR if seen (caller needs to block SIGINT) */
        COPY_SIGTERM       = 1 << 7,  /* ditto, but for SIGTERM */
        COPY_MAC_CREATE    = 1 << 8,  /* Create files with the correct MAC label (currently SELinux only) */
        COPY_HARDLINKS     = 1 << 9,  /* Try to reproduce hard links */
        COPY_FSYNC         = 1 << 10, /* fsync() after we are done */
        COPY_FSYNC_FULL    = 1 << 11, /* fsync_full() after we are done */
        COPY_SYNCFS        = 1 << 12, /* syncfs() the *top-level* dir after we are done */
        COPY_ALL_XATTRS    = 1 << 13, /* Preserve all xattrs when copying, not just those in the user namespace */
        COPY_HOLES         = 1 << 14, /* Copy holes */
        COPY_GRACEFUL_WARN = 1 << 15, /* Skip copying file types that aren't supported by the target filesystem */
} CopyFlags;

typedef enum DenyType {
        DENY_DONT,
        DENY_INODE,
        DENY_CONTENTS,
        _DENY_TYPE_MAX,
        _DENY_TYPE_INVALID = -EINVAL,
} DenyType;

typedef int (*copy_progress_bytes_t)(uint64_t n_bytes, void *userdata);
typedef int (*copy_progress_path_t)(const char *path, const struct stat *st, void *userdata);

int copy_file_fd_full(const char *from, int to, CopyFlags copy_flags, copy_progress_bytes_t progress, void *userdata);
static inline int copy_file_fd(const char *from, int to, CopyFlags copy_flags) {
        return copy_file_fd_full(from, to, copy_flags, NULL, NULL);
}

int copy_file_at_full(int dir_fdf, const char *from, int dir_fdt, const char *to, int open_flags, mode_t mode, unsigned chattr_flags, unsigned chattr_mask, CopyFlags copy_flags, copy_progress_bytes_t progress, void *userdata);
static inline int copy_file_at(int dir_fdf, const char *from, int dir_fdt, const char *to, int open_flags, mode_t mode, unsigned chattr_flags, unsigned chattr_mask, CopyFlags copy_flags) {
        return copy_file_at_full(dir_fdf, from, dir_fdt, to, open_flags, mode, chattr_flags, chattr_mask, copy_flags, NULL, NULL);
}
static inline int copy_file_full(const char *from, const char *to, int open_flags, mode_t mode, unsigned chattr_flags, unsigned chattr_mask, CopyFlags copy_flags, copy_progress_bytes_t progress, void *userdata) {
        return copy_file_at_full(AT_FDCWD, from, AT_FDCWD, to, open_flags, mode, chattr_flags, chattr_mask, copy_flags, progress, userdata);
}
static inline int copy_file(const char *from, const char *to, int open_flags, mode_t mode, unsigned chattr_flags, unsigned chattr_mask, CopyFlags copy_flags) {
        return copy_file_at(AT_FDCWD, from, AT_FDCWD, to, open_flags, mode, chattr_flags, chattr_mask, copy_flags);
}

int copy_file_atomic_full(const char *from, const char *to, mode_t mode, unsigned chattr_flags, unsigned chattr_mask, CopyFlags copy_flags, copy_progress_bytes_t progress, void *userdata);
static inline int copy_file_atomic(const char *from, const char *to, mode_t mode, unsigned chattr_flags, unsigned chattr_mask, CopyFlags copy_flags) {
        return copy_file_atomic_full(from, to, mode, chattr_flags, chattr_mask, copy_flags, NULL, NULL);
}

int copy_tree_at_full(int fdf, const char *from, int fdt, const char *to, uid_t override_uid, gid_t override_gid, CopyFlags copy_flags, Hashmap *denylist, copy_progress_path_t progress_path, copy_progress_bytes_t progress_bytes, void *userdata);
static inline int copy_tree_at(int fdf, const char *from, int fdt, const char *to, uid_t override_uid, gid_t override_gid, CopyFlags copy_flags, Hashmap *denylist) {
        return copy_tree_at_full(fdf, from, fdt, to, override_uid, override_gid, copy_flags, denylist, NULL, NULL, NULL);
}
static inline int copy_tree(const char *from, const char *to, uid_t override_uid, gid_t override_gid, CopyFlags copy_flags, Hashmap *denylist) {
        return copy_tree_at_full(AT_FDCWD, from, AT_FDCWD, to, override_uid, override_gid, copy_flags, denylist, NULL, NULL, NULL);
}

int copy_directory_fd_full(int dirfd, const char *to, CopyFlags copy_flags, copy_progress_path_t progress_path, copy_progress_bytes_t progress_bytes, void *userdata);
static inline int copy_directory_fd(int dirfd, const char *to, CopyFlags copy_flags) {
        return copy_directory_fd_full(dirfd, to, copy_flags, NULL, NULL, NULL);
}

int copy_directory_full(const char *from, const char *to, CopyFlags copy_flags, copy_progress_path_t progress_path, copy_progress_bytes_t progress_bytes, void *userdata);
static inline int copy_directory(const char *from, const char *to, CopyFlags copy_flags) {
        return copy_directory_full(from, to, copy_flags, NULL, NULL, NULL);
}

int copy_bytes_full(int fdf, int fdt, uint64_t max_bytes, CopyFlags copy_flags, void **ret_remains, size_t *ret_remains_size, copy_progress_bytes_t progress, void *userdata);
static inline int copy_bytes(int fdf, int fdt, uint64_t max_bytes, CopyFlags copy_flags) {
        return copy_bytes_full(fdf, fdt, max_bytes, copy_flags, NULL, NULL, NULL, NULL);
}

int copy_times(int fdf, int fdt, CopyFlags flags);
int copy_access(int fdf, int fdt);
int copy_rights_with_fallback(int fdf, int fdt, const char *patht);
static inline int copy_rights(int fdf, int fdt) {
        return copy_rights_with_fallback(fdf, fdt, NULL); /* no fallback */
}
int copy_xattr(int df, const char *from, int dt, const char *to, CopyFlags copy_flags);
