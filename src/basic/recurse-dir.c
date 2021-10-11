/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "dirent-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "missing_syscall.h"
#include "mountpoint-util.h"
#include "recurse-dir.h"
#include "sort-util.h"

#define DEFAULT_RECURSION_MAX 100

static int sort_func(struct dirent * const *a, struct dirent * const *b) {
        return strcmp((*a)->d_name, (*b)->d_name);
}

static bool ignore_dirent(const struct dirent *de, RecurseDirFlags flags) {
        assert(de);

        /* Depending on flag either ignore everything starting with ".", or just "." itself and ".." */

        return FLAGS_SET(flags, RECURSE_DIR_IGNORE_DOT) ?
                de->d_name[0] == '.' :
                dot_or_dot_dot(de->d_name);
}

/* Maximum space one direent structure might require at most */
#define DIRENT_SIZE_MAX MAX(sizeof(struct dirent), offsetof(struct dirent, d_name) + NAME_MAX + 1)

int readdir_all(int dir_fd,
                RecurseDirFlags flags,
                DirectoryEntries **ret) {

        _cleanup_free_ DirectoryEntries *de = NULL;
        DirectoryEntries *nde;
        size_t add, sz, j;

        assert(dir_fd >= 0);

        /* Returns an array with pointers to "struct dirent" directory entries, optionally sorted. Free the
         * array with readdir_all_freep(). */

        /* Only if 64bit off_t is enabled struct dirent + struct dirent64 are actually the same. We require
         * this, and we want them to be interchangable, hence verify that. */
        assert_cc(_FILE_OFFSET_BITS == 64);
        assert_cc(sizeof(struct dirent) == sizeof(struct dirent64));
        assert_cc(offsetof(struct dirent, d_ino) == offsetof(struct dirent64, d_ino));
        assert_cc(sizeof(((struct dirent*) NULL)->d_ino) == sizeof(((struct dirent64*) NULL)->d_ino));
        assert_cc(offsetof(struct dirent, d_off) == offsetof(struct dirent64, d_off));
        assert_cc(sizeof(((struct dirent*) NULL)->d_off) == sizeof(((struct dirent64*) NULL)->d_off));
        assert_cc(offsetof(struct dirent, d_reclen) == offsetof(struct dirent64, d_reclen));
        assert_cc(sizeof(((struct dirent*) NULL)->d_reclen) == sizeof(((struct dirent64*) NULL)->d_reclen));
        assert_cc(offsetof(struct dirent, d_type) == offsetof(struct dirent64, d_type));
        assert_cc(sizeof(((struct dirent*) NULL)->d_type) == sizeof(((struct dirent64*) NULL)->d_type));
        assert_cc(offsetof(struct dirent, d_name) == offsetof(struct dirent64, d_name));
        assert_cc(sizeof(((struct dirent*) NULL)->d_name) == sizeof(((struct dirent64*) NULL)->d_name));

        /* Start with space for up to 8 directory entries. We expect at least 2 ("." + ".."), hence hopefully
         * 8 will cover most cases comprehensively. (Note that most likely a lot more entries will actually
         * fit in the buffer, given we calculate maximum file name length here.) */
        de = malloc(offsetof(DirectoryEntries, buffer) + DIRENT_SIZE_MAX * 8);
        if (!de)
                return -ENOMEM;

        de->buffer_size = 0;
        for (;;) {
                size_t bs;
                ssize_t n;

                bs = MIN(MALLOC_SIZEOF_SAFE(de) - offsetof(DirectoryEntries, buffer), (size_t) SSIZE_MAX);
                assert(bs > de->buffer_size);

                n = getdents64(dir_fd, de->buffer + de->buffer_size, bs - de->buffer_size);
                if (n < 0)
                        return -errno;
                if (n == 0)
                        break;

                de->buffer_size += n;

                if (de->buffer_size < bs - DIRENT_SIZE_MAX) /* Still room for one more entry, then try to
                                                             * fill it up without growing the structure. */
                        continue;

                if (bs >= SSIZE_MAX - offsetof(DirectoryEntries, buffer))
                        return -EFBIG;
                bs = bs >= (SSIZE_MAX - offsetof(DirectoryEntries, buffer))/2 ? SSIZE_MAX - offsetof(DirectoryEntries, buffer) : bs * 2;

                nde = realloc(de, bs);
                if (!nde)
                        return -ENOMEM;

                de = nde;
        }

        de->n_entries = 0;
        for (struct dirent *entry = (struct dirent*) de->buffer;
             (uint8_t*) entry < de->buffer + de->buffer_size;
             entry = (struct dirent*) ((uint8_t*) entry + entry->d_reclen)) {

                if (ignore_dirent(entry, flags))
                        continue;

                de->n_entries++;
        }

        sz = ALIGN(offsetof(DirectoryEntries, buffer) + de->buffer_size);
        add = sizeof(struct dirent*) * de->n_entries;
        if (add > SIZE_MAX - add)
                return -ENOMEM;

        nde = realloc(de, sz + add);
        if (!nde)
                return -ENOMEM;

        de = nde;
        de->entries = (struct dirent**) ((uint8_t*) de + ALIGN(offsetof(DirectoryEntries, buffer) + de->buffer_size));

        j = 0;
        for (struct dirent *entry = (struct dirent*) de->buffer;
             (uint8_t*) entry < de->buffer + de->buffer_size;
             entry = (struct dirent*) ((uint8_t*) entry + entry->d_reclen)) {

                if (ignore_dirent(entry, flags))
                        continue;

                de->entries[j++] = entry;
        }

        if (FLAGS_SET(flags, RECURSE_DIR_SORT))
                typesafe_qsort(de->entries, de->n_entries, sort_func);

        if (ret)
                *ret = TAKE_PTR(de);

        return 0;
}

int recurse_dir(
                int dir_fd,
                const char *path,
                unsigned statx_mask,
                unsigned n_depth_max,
                RecurseDirFlags flags,
                recurse_dir_func_t func,
                void *userdata) {

        _cleanup_free_ DirectoryEntries *de = NULL;
        int r;

        assert(dir_fd >= 0);
        assert(func);

        /* This is a lot like ftw()/nftw(), but a lot more modern, i.e. built around openat()/statx()/O_PATH,
         * and under the assumption that fds are not as 'expensive' as they used to be. */

        if (n_depth_max == 0)
                return -EOVERFLOW;
        if (n_depth_max == UINT_MAX) /* special marker for "default" */
                n_depth_max = DEFAULT_RECURSION_MAX;

        r = readdir_all(dir_fd, flags, &de);
        if (r < 0)
                return r;

        for (size_t i = 0; i < de->n_entries; i++) {
                _cleanup_close_ int inode_fd = -1, subdir_fd = -1;
                _cleanup_free_ char *joined = NULL;
                STRUCT_STATX_DEFINE(sx);
                bool sx_valid = false;
                const char *p;

                /* For each directory entry we'll do one of the following:
                 *
                 * 1) If the entry refers to a directory, we'll open it as O_DIRECTORY 'subdir_fd' and then statx() the opened directory via that new fd (if requested)
                 * 2) Otherwise, if RECURSE_DIR_INODE_FD is set we'll it as open O_PATH 'inode_fd' and then statx() the opened inode via that new fd (if requested)
                 * 3) Otherwise, we'll statx() the directory entry via the directory fd we are currently looking at (if requested)
                 */

                if (path) {
                        joined = path_join(path, de->entries[i]->d_name);
                        if (!joined)
                                return -ENOMEM;

                        p = joined;
                } else
                        p = de->entries[i]->d_name;

                if (IN_SET(de->entries[i]->d_type, DT_UNKNOWN, DT_DIR)) {
                        subdir_fd = openat(dir_fd, de->entries[i]->d_name, O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
                        if (subdir_fd < 0) {
                                if (errno == ENOENT) /* Vanished by now, go for next file immediately */
                                        continue;

                                /* If it is a subdir but we failed to open it, then fail */
                                if (!IN_SET(errno, ENOTDIR, ELOOP)) {
                                        log_debug_errno(errno, "Failed to open directory '%s': %m", p);

                                        assert(errno <= RECURSE_DIR_SKIP_OPEN_DIR_ERROR_MAX - RECURSE_DIR_SKIP_OPEN_DIR_ERROR_BASE);

                                        r = func(RECURSE_DIR_SKIP_OPEN_DIR_ERROR_BASE + errno,
                                                 p,
                                                 dir_fd,
                                                 -1,
                                                 de->entries[i],
                                                 NULL,
                                                 userdata);
                                        if (r == RECURSE_DIR_LEAVE_DIRECTORY)
                                                break;
                                        if (!IN_SET(r, RECURSE_DIR_CONTINUE, RECURSE_DIR_SKIP_ENTRY))
                                                return r;

                                        continue;
                                }

                                /* If it's not a subdir, then let's handle it like a regular inode below */

                        } else {
                                /* If we managed to get a DIR* off the inode, it's definitely a directory. */
                                de->entries[i]->d_type = DT_DIR;

                                if (statx_mask != 0 || (flags & RECURSE_DIR_SAME_MOUNT)) {
                                        r = statx_fallback(subdir_fd, "", AT_EMPTY_PATH, statx_mask, &sx);
                                        if (r < 0)
                                                return r;

                                        sx_valid = true;
                                }
                        }
                }

                if (subdir_fd < 0) {
                        /* It's not a subdirectory. */

                        if (flags & RECURSE_DIR_INODE_FD) {

                                inode_fd = openat(dir_fd, de->entries[i]->d_name, O_PATH|O_NOFOLLOW|O_CLOEXEC);
                                if (inode_fd < 0) {
                                        if (errno == ENOENT) /* Vanished by now, go for next file immediately */
                                                continue;

                                        log_debug_errno(errno, "Failed to open directory entry '%s': %m", p);

                                        assert(errno <= RECURSE_DIR_SKIP_OPEN_INODE_ERROR_MAX - RECURSE_DIR_SKIP_OPEN_INODE_ERROR_BASE);

                                        r = func(RECURSE_DIR_SKIP_OPEN_INODE_ERROR_BASE + errno,
                                                 p,
                                                 dir_fd,
                                                 -1,
                                                 de->entries[i],
                                                 NULL,
                                                 userdata);
                                        if (r == RECURSE_DIR_LEAVE_DIRECTORY)
                                                break;
                                        if (!IN_SET(r, RECURSE_DIR_CONTINUE, RECURSE_DIR_SKIP_ENTRY))
                                                return r;

                                        continue;
                                }

                                /* If we open the inode, then verify it's actually a non-directory, like we
                                 * assume. Let's guarantee that we never pass statx data of a directory where
                                 * caller expects a non-directory */

                                r = statx_fallback(inode_fd, "", AT_EMPTY_PATH, statx_mask | STATX_TYPE, &sx);
                                if (r < 0)
                                        return r;

                                assert(sx.stx_mask & STATX_TYPE);
                                sx_valid = true;

                                if (S_ISDIR(sx.stx_mode)) {
                                        /* What? It's a directory now? Then someone must have quickly
                                         * replaced it. Let's handle that gracefully: convert it to a
                                         * directory fd — which sould be riskless now that we pinned the
                                         * inode. */

                                        subdir_fd = openat(AT_FDCWD, FORMAT_PROC_FD_PATH(inode_fd), O_DIRECTORY|O_CLOEXEC);
                                        if (subdir_fd < 0)
                                                return -errno;

                                        inode_fd = safe_close(inode_fd);
                                }

                        } else if (statx_mask != 0 || (de->entries[i]->d_type == DT_UNKNOWN && (flags & RECURSE_DIR_ENSURE_TYPE))) {

                                r = statx_fallback(dir_fd, de->entries[i]->d_name, AT_SYMLINK_NOFOLLOW, statx_mask | STATX_TYPE, &sx);
                                if (r == -ENOENT) /* Vanished by now? Go for next file immediately */
                                        continue;
                                if (r < 0) {
                                        log_debug_errno(r, "Failed to stat directory entry '%s': %m", p);

                                        assert(errno <= RECURSE_DIR_SKIP_STAT_INODE_ERROR_MAX - RECURSE_DIR_SKIP_STAT_INODE_ERROR_BASE);

                                        r = func(RECURSE_DIR_SKIP_STAT_INODE_ERROR_BASE + -r,
                                                 p,
                                                 dir_fd,
                                                 -1,
                                                 de->entries[i],
                                                 NULL,
                                                 userdata);
                                        if (r == RECURSE_DIR_LEAVE_DIRECTORY)
                                                break;
                                        if (!IN_SET(r, RECURSE_DIR_CONTINUE, RECURSE_DIR_SKIP_ENTRY))
                                                return r;

                                        continue;
                                }

                                assert(sx.stx_mask & STATX_TYPE);
                                sx_valid = true;

                                if (S_ISDIR(sx.stx_mode)) {
                                        /* So it suddenly is a directory, but we couldn't open it as such
                                         * earlier?  That is weird, and probably means somebody is racing
                                         * against us. We could of course retry and open it as a directory
                                         * again, but the chance to win here is limited. Hence, let's
                                         * propagate this as EISDIR error instead. That way we make this
                                         * something that can be reasonably handled, even though we give the
                                         * guarantee that RECURSE_DIR_ENTRY is strictly issued for
                                         * non-directory dirents. */

                                        log_debug_errno(r, "Non-directory entry '%s' suddenly became a directory: %m", p);

                                        r = func(RECURSE_DIR_SKIP_STAT_INODE_ERROR_BASE + EISDIR,
                                                 p,
                                                 dir_fd,
                                                 -1,
                                                 de->entries[i],
                                                 NULL,
                                                 userdata);
                                        if (r == RECURSE_DIR_LEAVE_DIRECTORY)
                                                break;
                                        if (!IN_SET(r, RECURSE_DIR_CONTINUE, RECURSE_DIR_SKIP_ENTRY))
                                                return r;

                                        continue;
                                }
                        }
                }

                if (sx_valid) {
                        /* Copy over the data we acquired through statx() if we acquired any */
                        if (sx.stx_mask & STATX_TYPE) {
                                assert((subdir_fd < 0) == !S_ISDIR(sx.stx_mode));
                                de->entries[i]->d_type = IFTODT(sx.stx_mode);
                        }

                        if (sx.stx_mask & STATX_INO)
                                de->entries[i]->d_ino = sx.stx_ino;
                }

                if (subdir_fd >= 0) {
                        if (FLAGS_SET(flags, RECURSE_DIR_SAME_MOUNT)) {
                                bool is_mount;

                                if (sx_valid && FLAGS_SET(sx.stx_attributes_mask, STATX_ATTR_MOUNT_ROOT))
                                        is_mount = FLAGS_SET(sx.stx_attributes, STATX_ATTR_MOUNT_ROOT);
                                else {
                                        r = fd_is_mount_point(dir_fd, de->entries[i]->d_name, 0);
                                        if (r < 0)
                                                log_debug_errno(r, "Failed to determine whether %s is a submount, assuming not: %m", p);

                                        is_mount = r > 0;
                                }

                                if (is_mount) {
                                        r = func(RECURSE_DIR_SKIP_MOUNT,
                                                 p,
                                                 dir_fd,
                                                 subdir_fd,
                                                 de->entries[i],
                                                 statx_mask != 0 ? &sx : NULL, /* only pass sx if user asked for it */
                                                 userdata);
                                        if (r == RECURSE_DIR_LEAVE_DIRECTORY)
                                                break;
                                        if (!IN_SET(r, RECURSE_DIR_CONTINUE, RECURSE_DIR_SKIP_ENTRY))
                                                return r;

                                        continue;
                                }
                        }

                        if (n_depth_max <= 1) {
                                /* When we reached max depth, generate a special event */

                                r = func(RECURSE_DIR_SKIP_DEPTH,
                                         p,
                                         dir_fd,
                                         subdir_fd,
                                         de->entries[i],
                                         statx_mask != 0 ? &sx : NULL, /* only pass sx if user asked for it */
                                         userdata);
                                if (r == RECURSE_DIR_LEAVE_DIRECTORY)
                                        break;
                                if (!IN_SET(r, RECURSE_DIR_CONTINUE, RECURSE_DIR_SKIP_ENTRY))
                                        return r;

                                continue;
                        }

                        r = func(RECURSE_DIR_ENTER,
                                 p,
                                 dir_fd,
                                 subdir_fd,
                                 de->entries[i],
                                 statx_mask != 0 ? &sx : NULL, /* only pass sx if user asked for it */
                                 userdata);
                        if (r == RECURSE_DIR_LEAVE_DIRECTORY)
                                break;
                        if (r == RECURSE_DIR_SKIP_ENTRY)
                                continue;
                        if (r != RECURSE_DIR_CONTINUE)
                                return r;

                        r = recurse_dir(subdir_fd,
                                        p,
                                        statx_mask,
                                        n_depth_max - 1,
                                        flags,
                                        func,
                                        userdata);
                        if (r != 0)
                                return r;

                        r = func(RECURSE_DIR_LEAVE,
                                 p,
                                 dir_fd,
                                 subdir_fd,
                                 de->entries[i],
                                 statx_mask != 0 ? &sx : NULL, /* only pass sx if user asked for it */
                                 userdata);
                } else
                        /* Non-directory inode */
                        r = func(RECURSE_DIR_ENTRY,
                                 p,
                                 dir_fd,
                                 inode_fd,
                                 de->entries[i],
                                 statx_mask != 0 ? &sx : NULL, /* only pass sx if user asked for it */
                                 userdata);


                if (r == RECURSE_DIR_LEAVE_DIRECTORY)
                        break;
                if (!IN_SET(r, RECURSE_DIR_SKIP_ENTRY, RECURSE_DIR_CONTINUE))
                        return r;
        }

        return 0;
}

int recurse_dir_at(
                int atfd,
                const char *path,
                unsigned statx_mask,
                unsigned n_depth_max,
                RecurseDirFlags flags,
                recurse_dir_func_t func,
                void *userdata) {

        _cleanup_close_ int fd = -1;

        assert(atfd >= 0 || atfd == AT_FDCWD);
        assert(func);

        if (!path)
                path = ".";

        fd = openat(atfd, path, O_DIRECTORY|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        return recurse_dir(fd, path, statx_mask, n_depth_max, flags, func, userdata);
}
