/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <linux/btrfs.h>
#include <sys/ioctl.h>

#include "btrfs.h"
#include "fd-util.h"
#include "fs-util.h"
#include "path-util.h"

int btrfs_validate_subvolume_name(const char *name) {

        if (!filename_is_valid(name))
                return -EINVAL;

        if (strlen(name) > BTRFS_SUBVOL_NAME_MAX)
                return -E2BIG;

        return 0;
}

static int extract_subvolume_name(const char *path, char **ret) {
        _cleanup_free_ char *fn = NULL;
        int r;

        assert(path);
        assert(ret);

        r = path_extract_filename(path, &fn);
        if (r < 0)
                return r;

        r = btrfs_validate_subvolume_name(fn);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(fn);
        return 0;
}

int btrfs_subvol_make(int dir_fd, const char *path) {
        struct btrfs_ioctl_vol_args args = {};
        _cleanup_free_ char *subvolume = NULL;
        _cleanup_close_ int fd = -EBADF;
        int r;

        assert(dir_fd >= 0 || dir_fd == AT_FDCWD);
        assert(!isempty(path));

        r = extract_subvolume_name(path, &subvolume);
        if (r < 0)
                return r;

        r = path_extract_directory(path, NULL);
        if (r >= 0) {
                fd = open_parent_at(dir_fd, path, O_RDONLY|O_CLOEXEC|O_CLOEXEC, 0);
                if (fd < 0)
                        return fd;

                dir_fd = fd;
        }

        strncpy(args.name, subvolume, sizeof(args.name)-1);

        return RET_NERRNO(ioctl(dir_fd, BTRFS_IOC_SUBVOL_CREATE, &args));
}

int btrfs_subvol_make_fallback(int dir_fd, const char *path, mode_t mode) {
        mode_t old, combined;
        int r;

        assert(path);

        /* Let's work like mkdir(), i.e. take the specified mode, and mask it with the current umask. */
        old = umask(~mode);
        combined = old | ~mode;
        if (combined != ~mode)
                umask(combined);
        r = btrfs_subvol_make(dir_fd, path);
        umask(old);

        if (r >= 0)
                return 1; /* subvol worked */
        if (r != -ENOTTY)
                return r;

        if (mkdirat(dir_fd, path, mode) < 0)
                return -errno;

        return 0; /* plain directory */
}
