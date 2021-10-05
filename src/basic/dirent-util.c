/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <sys/stat.h>

#include "dirent-util.h"
#include "path-util.h"
#include "string-util.h"

int stat_mode_to_dirent_type(mode_t mode) {
        return
                S_ISREG(mode)  ? DT_REG  :
                S_ISDIR(mode)  ? DT_DIR  :
                S_ISLNK(mode)  ? DT_LNK  :
                S_ISFIFO(mode) ? DT_FIFO :
                S_ISSOCK(mode) ? DT_SOCK :
                S_ISCHR(mode)  ? DT_CHR  :
                S_ISBLK(mode)  ? DT_BLK  :
                                 DT_UNKNOWN;
}

static int dirent_ensure_type(DIR *d, struct dirent *de) {
        struct stat st;

        assert(d);
        assert(de);

        if (de->d_type != DT_UNKNOWN)
                return 0;

        if (dot_or_dot_dot(de->d_name)) {
                de->d_type = DT_DIR;
                return 0;
        }

        if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0)
                return -errno;

        de->d_type = stat_mode_to_dirent_type(st.st_mode);

        return 0;
}

bool dirent_is_file(const struct dirent *de) {
        assert(de);

        if (!IN_SET(de->d_type, DT_REG, DT_LNK, DT_UNKNOWN))
                return false;

        if (hidden_or_backup_file(de->d_name))
                return false;

        return true;
}

bool dirent_is_file_with_suffix(const struct dirent *de, const char *suffix) {
        assert(de);

        if (!IN_SET(de->d_type, DT_REG, DT_LNK, DT_UNKNOWN))
                return false;

        if (de->d_name[0] == '.')
                return false;

        if (!suffix)
                return true;

        return endswith(de->d_name, suffix);
}

struct dirent *readdir_ensure_type(DIR *d) {
        struct dirent *de;

        assert(d);

        errno = 0;
        de = readdir(d);
        if (de)
                (void) dirent_ensure_type(d, de);
        return de;
}

struct dirent *readdir_no_dot(DIR *dirp) {
        struct dirent *d;

        for (;;) {
                d = readdir_ensure_type(dirp);
                if (d && dot_or_dot_dot(d->d_name))
                        continue;
                return d;
        }
}
