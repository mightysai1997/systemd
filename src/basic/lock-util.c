/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "alloc-util.h"
#include "fd-util.h"
#include "fs-util.h"
#include "lock-util.h"
#include "macro.h"
#include "missing_fcntl.h"
#include "path-util.h"

int make_lock_file_at(int dir_fd, const char *p, int operation, LockFile *ret) {
        _cleanup_close_ int fd = -EBADF, dfd = -EBADF;
        _cleanup_free_ char *t = NULL;
        int r;

        assert(dir_fd >= 0 || dir_fd == AT_FDCWD);
        assert(p);
        assert(IN_SET(operation & ~LOCK_NB, LOCK_EX, LOCK_SH));
        assert(ret);

        if (isempty(p))
                return -EINVAL;

        /* We use UNPOSIX locks as they have nice semantics, and are mostly compatible with NFS. */

        dfd = fd_reopen(dir_fd, O_CLOEXEC|O_PATH|O_DIRECTORY);
        if (dfd < 0)
                return dfd;

        t = strdup(p);
        if (!t)
                return -ENOMEM;

        for (;;) {
                struct stat st;

                fd = openat(dfd, p, O_CREAT|O_RDWR|O_NOFOLLOW|O_CLOEXEC|O_NOCTTY, 0600);
                if (fd < 0)
                        return -errno;

                r = unposix_lock(fd, operation);
                if (r < 0)
                        return r == -EAGAIN ? -EBUSY : r;

                /* If we acquired the lock, let's check if the file still exists in the file system. If not,
                 * then the previous exclusive owner removed it and then closed it. In such a case our
                 * acquired lock is worthless, hence try again. */

                if (fstat(fd, &st) < 0)
                        return -errno;
                if (st.st_nlink > 0)
                        break;

                fd = safe_close(fd);
        }

        *ret = (LockFile) {
                .dir_fd = TAKE_FD(dfd),
                .path = TAKE_PTR(t),
                .fd = TAKE_FD(fd),
                .operation = operation,
        };

        return 0;
}

int make_lock_file_for(const char *p, int operation, LockFile *ret) {
        _cleanup_free_ char *fn = NULL, *dn = NULL, *t = NULL;
        int r;

        assert(p);
        assert(ret);

        r = path_extract_filename(p, &fn);
        if (r < 0)
                return r;

        r = path_extract_directory(p, &dn);
        if (r < 0)
                return r;

        t = strjoin(dn, "/.#", fn, ".lck");
        if (!t)
                return -ENOMEM;

        return make_lock_file(t, operation, ret);
}

void release_lock_file(LockFile *f) {
        if (!f)
                return;

        if (f->path) {

                /* If we are the exclusive owner we can safely delete
                 * the lock file itself. If we are not the exclusive
                 * owner, we can try becoming it. */

                if (f->fd >= 0 &&
                    (f->operation & ~LOCK_NB) == LOCK_SH &&
                    unposix_lock(f->fd, LOCK_EX|LOCK_NB) >= 0)
                        f->operation = LOCK_EX|LOCK_NB;

                if ((f->operation & ~LOCK_NB) == LOCK_EX)
                        (void) unlinkat(f->dir_fd, f->path, 0);

                f->path = mfree(f->path);
        }

        f->dir_fd = safe_close(f->dir_fd);
        f->fd = safe_close(f->fd);
        f->operation = 0;
}

int unposix_lock(int fd, int operation) {
        int cmd, type, r;

        assert(fd >= 0);

        cmd = (operation & LOCK_NB) ? F_OFD_SETLK : F_OFD_SETLKW;

        switch (operation & ~LOCK_NB) {
                case LOCK_EX:
                        type = F_WRLCK;
                        break;
                case LOCK_SH:
                        type = F_RDLCK;
                        break;
                case LOCK_UN:
                        type = F_UNLCK;
                        break;
                default:
                        assert_not_reached();
        }

        r = RET_NERRNO(fcntl(fd, cmd, &(struct flock) {
                .l_type = type,
                .l_whence = SEEK_SET,
                .l_start = 0,
                .l_len = 0,
        }));

        if (r == -EACCES) /* Treat EACCESS/EAGAIN the same as per man page. */
                r = -EAGAIN;

        return r;
}

void unposix_unlockpp(int **fd) {
        assert(fd);

        if (!*fd || **fd < 0)
                return;

        (void) unposix_lock(**fd, LOCK_UN);
        *fd = NULL;
}
