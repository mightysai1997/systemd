/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <sys/types.h>

typedef enum namespace_type {
        NAMESPACE_USER          = 0,
        NAMESPACE_MNT           = 1,
        NAMESPACE_PID           = 2,
        NAMESPACE_UTS           = 3,
        NAMESPACE_IPC           = 4,
        NAMESPACE_NET           = 5,
        NAMESPACE_CGROUP        = 6,
        NAMESPACE_TIME          = 7,
        _NAMESPACE_TYPE_MAX     = 8,
        _NAMESPACE_TYPE_INVALID = -EINVAL,
} namespace_type;

extern const struct namespace_info {
        const char *proc_name;
        const char *proc_path;
        unsigned int clone_flag;
} namespace_info[_NAMESPACE_TYPE_MAX + 1];

int namespace_open(pid_t pid, int *pidns_fd, int *mntns_fd, int *netns_fd, int *userns_fd, int *root_fd);
int namespace_enter(int pidns_fd, int mntns_fd, int netns_fd, int userns_fd, int root_fd);

int fd_is_ns(int fd, unsigned long nsflag);

int detach_mount_namespace(void);

static inline bool userns_shift_range_valid(uid_t shift, uid_t range) {
        /* Checks that the specified userns range makes sense, i.e. contains at least one UID, and the end
         * doesn't overflow uid_t. */

        assert_cc((uid_t) -1 > 0); /* verify that uid_t is unsigned */

        if (range <= 0)
                return false;

        if (shift > (uid_t) -1 - range)
                return false;

        return true;
}

int userns_acquire(const char *uid_map, const char *gid_map);
int in_same_namespace(pid_t pid1, pid_t pid2, namespace_type ns);
