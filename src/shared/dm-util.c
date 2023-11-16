/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <fcntl.h>
#include <linux/dm-ioctl.h>
#include <sys/ioctl.h>

#include "dm-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "string-util.h"

int dm_deferred_remove_cancel(const char *name) {
        _cleanup_close_ int fd = -EBADF;
        struct message {
                struct dm_ioctl dm_ioctl;
                struct dm_target_msg dm_target_msg;
                char msg_text[STRLEN("@cancel_deferred_remove") + 1];
        } _packed_ message = {
                .dm_ioctl = {
                        .version = {
                                DM_VERSION_MAJOR,
                                DM_VERSION_MINOR,
                                DM_VERSION_PATCHLEVEL
                        },
                        .data_size = sizeof(struct message),
                        .data_start = sizeof(struct dm_ioctl),
                },
                .msg_text = "@cancel_deferred_remove",
        };

        assert(name);

        if (strlen(name) >= sizeof(message.dm_ioctl.name))
                return -ENODEV; /* A device with a name longer than this cannot possibly exist */

        strncpy_exact(message.dm_ioctl.name, name, sizeof(message.dm_ioctl.name));

        fd = open("/dev/mapper/control", O_RDWR|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        if (ioctl(fd, DM_TARGET_MSG, &message))
                return -errno;

        return 0;
}

static int dm_do_ioctl(const char *name, int cmd, struct dm_ioctl *dmi) {
        _cleanup_close_ int fd = -EBADF;
        int r;

        assert(name);
        assert(dmi);

        dmi->version[0] = DM_VERSION_MAJOR;
        dmi->version[1] = DM_VERSION_MINOR;
        dmi->version[2] = DM_VERSION_PATCHLEVEL;

        if (strlen(name) >= DM_NAME_LEN)
                return -ENODEV;

        strcpy(dmi->name, name);

        fd = open("/dev/mapper/control", O_RDWR|O_CLOEXEC);
        if (fd < 0)
                return -errno;

        r = RET_NERRNO(ioctl(fd, cmd, dmi));
        if (r < 0)
                return r;

        assert(streq(dmi->name, name));
        return 0;
}

int dm_get_devnode(const char *name, char **ret_devnode) {
        char buf[STRLEN("/dev/dm-") + DECIMAL_STR_MAX(dev_t)]; /* DECIMAL_STR_MAX includes space for a trailing NUL */
        struct dm_ioctl dmi = {
                .data_size = sizeof(struct dm_ioctl),
        };
        char *p;
        int r;

        assert(name);
        assert(ret_devnode);

        r = dm_do_ioctl(name, DM_DEV_STATUS, &dmi);
        if (r < 0)
                return r;

        xsprintf(buf, "/dev/dm-%u", minor(dmi.dev));

        p = strdup(buf);
        if (!p)
                return -ENOMEM;

        *ret_devnode = TAKE_PTR(p);
        return 0;
}
