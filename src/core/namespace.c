/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>

#include "alloc-util.h"
#include "dev-setup.h"
#include "fd-util.h"
#include "loopback-setup.h"
#include "missing.h"
#include "mkdir.h"
#include "mount-util.h"
#include "namespace.h"
#include "path-util.h"
#include "selinux-util.h"
#include "socket-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "umask-util.h"
#include "user-util.h"
#include "util.h"

#define DEV_MOUNT_OPTIONS (MS_NOSUID|MS_STRICTATIME|MS_NOEXEC)

typedef enum MountMode {
        /* This is ordered by priority! */
        INACCESSIBLE,
        READONLY,
        PRIVATE_TMP,
        PRIVATE_VAR_TMP,
        PRIVATE_DEV,
        READWRITE,
} MountMode;

typedef struct BindMount {
        const char *path;
        MountMode mode;
        bool done;
        bool ignore;
} BindMount;

static int append_mounts(BindMount **p, char **strv, MountMode mode) {
        char **i;

        assert(p);

        STRV_FOREACH(i, strv) {

                (*p)->ignore = false;
                (*p)->done = false;

                if ((mode == INACCESSIBLE || mode == READONLY || mode == READWRITE) && (*i)[0] == '-') {
                        (*p)->ignore = true;
                        (*i)++;
                }

                if (!path_is_absolute(*i))
                        return -EINVAL;

                (*p)->path = *i;
                (*p)->mode = mode;
                (*p)++;
        }

        return 0;
}

static int mount_path_compare(const void *a, const void *b) {
        const BindMount *p = a, *q = b;
        int d;

        /* If the paths are not equal, then order prefixes first */
        d = path_compare(p->path, q->path);
        if (d != 0)
                return d;

        /* If the paths are equal, check the mode */
        if (p->mode < q->mode)
                return -1;

        if (p->mode > q->mode)
                return 1;

        return 0;
}

static void drop_duplicates(BindMount *m, unsigned *n) {
        BindMount *f, *t, *previous;

        assert(m);
        assert(n);

        /* Drops duplicate entries. Expects that the array is properly ordered already. */

        for (f = m, t = m, previous = NULL; f < m+*n; f++) {

                /* The first one wins (which is the one with the more restrictive mode), see mount_path_compare()
                 * above. */
                if (previous && path_equal(f->path, previous->path)) {
                        log_debug("%s is duplicate.", f->path);
                        continue;
                }

                *t = *f;
                previous = t;
                t++;
        }

        *n = t - m;
}

static void drop_inaccessible(BindMount *m, unsigned *n) {
        BindMount *f, *t;
        const char *clear = NULL;

        assert(m);
        assert(n);

        /* Drops all entries obstructed by another entry further up the tree. Expects that the array is properly
         * ordered already. */

        for (f = m, t = m; f < m+*n; f++) {

                /* If we found a path set for INACCESSIBLE earlier, and this entry has it as prefix we should drop
                 * it, as inaccessible paths really should drop the entire subtree. */
                if (clear && path_startswith(f->path, clear)) {
                        log_debug("%s is masked by %s.", f->path, clear);
                        continue;
                }

                clear = f->mode == INACCESSIBLE ? f->path : NULL;

                *t = *f;
                t++;
        }

        *n = t - m;
}

static void drop_nop(BindMount *m, unsigned *n) {
        BindMount *f, *t;

        assert(m);
        assert(n);

        /* Drops all entries which have an immediate parent that has the same type, as they are redundant. Assumes the
         * list is ordered by prefixes. */

        for (f = m, t = m; f < m+*n; f++) {

                /* Only suppress such subtrees for READONLY and READWRITE entries */
                if (IN_SET(f->mode, READONLY, READWRITE)) {
                        BindMount *p;
                        bool found = false;

                        /* Now let's find the first parent of the entry we are looking at. */
                        for (p = t-1; p >= m; p--) {
                                if (path_startswith(f->path, p->path)) {
                                        found = true;
                                        break;
                                }
                        }

                        /* We found it, let's see if it's the same mode, if so, we can drop this entry */
                        if (found && p->mode == f->mode) {
                                log_debug("%s is redundant by %s", f->path, p->path);
                                continue;
                        }
                }

                *t = *f;
                t++;
        }

        *n = t - m;
}

static int mount_dev(BindMount *m) {
        static const char devnodes[] =
                "/dev/null\0"
                "/dev/zero\0"
                "/dev/full\0"
                "/dev/random\0"
                "/dev/urandom\0"
                "/dev/tty\0";

        char temporary_mount[] = "/tmp/namespace-dev-XXXXXX";
        const char *d, *dev = NULL, *devpts = NULL, *devshm = NULL, *devhugepages = NULL, *devmqueue = NULL, *devlog = NULL, *devptmx = NULL;
        _cleanup_umask_ mode_t u;
        int r;

        assert(m);

        u = umask(0000);

        if (!mkdtemp(temporary_mount))
                return -errno;

        dev = strjoina(temporary_mount, "/dev");
        (void) mkdir(dev, 0755);
        if (mount("tmpfs", dev, "tmpfs", DEV_MOUNT_OPTIONS, "mode=755") < 0) {
                r = -errno;
                goto fail;
        }

        devpts = strjoina(temporary_mount, "/dev/pts");
        (void) mkdir(devpts, 0755);
        if (mount("/dev/pts", devpts, NULL, MS_BIND, NULL) < 0) {
                r = -errno;
                goto fail;
        }

        devptmx = strjoina(temporary_mount, "/dev/ptmx");
        if (symlink("pts/ptmx", devptmx) < 0) {
                r = -errno;
                goto fail;
        }

        devshm = strjoina(temporary_mount, "/dev/shm");
        (void) mkdir(devshm, 01777);
        r = mount("/dev/shm", devshm, NULL, MS_BIND, NULL);
        if (r < 0) {
                r = -errno;
                goto fail;
        }

        devmqueue = strjoina(temporary_mount, "/dev/mqueue");
        (void) mkdir(devmqueue, 0755);
        (void) mount("/dev/mqueue", devmqueue, NULL, MS_BIND, NULL);

        devhugepages = strjoina(temporary_mount, "/dev/hugepages");
        (void) mkdir(devhugepages, 0755);
        (void) mount("/dev/hugepages", devhugepages, NULL, MS_BIND, NULL);

        devlog = strjoina(temporary_mount, "/dev/log");
        (void) symlink("/run/systemd/journal/dev-log", devlog);

        NULSTR_FOREACH(d, devnodes) {
                _cleanup_free_ char *dn = NULL;
                struct stat st;

                r = stat(d, &st);
                if (r < 0) {

                        if (errno == ENOENT)
                                continue;

                        r = -errno;
                        goto fail;
                }

                if (!S_ISBLK(st.st_mode) &&
                    !S_ISCHR(st.st_mode)) {
                        r = -EINVAL;
                        goto fail;
                }

                if (st.st_rdev == 0)
                        continue;

                dn = strappend(temporary_mount, d);
                if (!dn) {
                        r = -ENOMEM;
                        goto fail;
                }

                mac_selinux_create_file_prepare(d, st.st_mode);
                r = mknod(dn, st.st_mode, st.st_rdev);
                mac_selinux_create_file_clear();

                if (r < 0) {
                        r = -errno;
                        goto fail;
                }
        }

        dev_setup(temporary_mount, UID_INVALID, GID_INVALID);

        /* Create the /dev directory if missing. It is more likely to be
         * missing when the service is started with RootDirectory. This is
         * consistent with mount units creating the mount points when missing.
         */
        (void) mkdir_p_label(m->path, 0755);

        /* Unmount everything in old /dev */
        umount_recursive(m->path, 0);
        if (mount(dev, m->path, NULL, MS_MOVE, NULL) < 0) {
                r = -errno;
                goto fail;
        }

        rmdir(dev);
        rmdir(temporary_mount);

        return 0;

fail:
        if (devpts)
                umount(devpts);

        if (devshm)
                umount(devshm);

        if (devhugepages)
                umount(devhugepages);

        if (devmqueue)
                umount(devmqueue);

        umount(dev);
        rmdir(dev);
        rmdir(temporary_mount);

        return r;
}

static int apply_mount(
                BindMount *m,
                const char *tmp_dir,
                const char *var_tmp_dir) {

        const char *what;
        int r;
        struct stat target;

        assert(m);

        log_debug("Applying namespace mount on %s", m->path);

        switch (m->mode) {

        case INACCESSIBLE:

                /* First, get rid of everything that is below if there
                 * is anything... Then, overmount it with an
                 * inaccessible path. */
                (void) umount_recursive(m->path, 0);

                if (lstat(m->path, &target) < 0) {
                        if (m->ignore && errno == ENOENT)
                                return 0;
                        return -errno;
                }

                what = mode_to_inaccessible_node(target.st_mode);
                if (!what) {
                        log_debug("File type not supported for inaccessible mounts. Note that symlinks are not allowed");
                        return -ELOOP;
                }
                break;

        case READONLY:
        case READWRITE:
                /* Nothing to mount here, we just later toggle the
                 * MS_RDONLY bit for the mount point */
                return 0;

        case PRIVATE_TMP:
                what = tmp_dir;
                break;

        case PRIVATE_VAR_TMP:
                what = var_tmp_dir;
                break;

        case PRIVATE_DEV:
                return mount_dev(m);

        default:
                assert_not_reached("Unknown mode");
        }

        assert(what);

        r = mount(what, m->path, NULL, MS_BIND|MS_REC, NULL);
        if (r >= 0) {
                log_debug("Successfully mounted %s to %s", what, m->path);
                return r;
        } else {
                if (m->ignore && errno == ENOENT)
                        return 0;
                return log_debug_errno(errno, "Failed to mount %s to %s: %m", what, m->path);
        }
}

static int make_read_only(BindMount *m) {
        int r;

        assert(m);

        if (IN_SET(m->mode, INACCESSIBLE, READONLY))
                r = bind_remount_recursive(m->path, true);
        else if (IN_SET(m->mode, READWRITE, PRIVATE_TMP, PRIVATE_VAR_TMP, PRIVATE_DEV)) {
                r = bind_remount_recursive(m->path, false);
                if (r == 0 && m->mode == PRIVATE_DEV) /* can be readonly but the submounts can't*/
                        if (mount(NULL, m->path, NULL, MS_REMOUNT|DEV_MOUNT_OPTIONS|MS_RDONLY, NULL) < 0)
                                r = -errno;
        } else
                r = 0;

        if (m->ignore && r == -ENOENT)
                return 0;

        return r;
}

int setup_namespace(
                const char* root_directory,
                char** read_write_paths,
                char** read_only_paths,
                char** inaccessible_paths,
                const char* tmp_dir,
                const char* var_tmp_dir,
                bool private_dev,
                bool protect_sysctl,
                bool protect_cgroups,
                ProtectHome protect_home,
                ProtectSystem protect_system,
                unsigned long mount_flags) {

        BindMount *m, *mounts = NULL;
        unsigned n;
        int r = 0;

        if (mount_flags == 0)
                mount_flags = MS_SHARED;

        if (unshare(CLONE_NEWNS) < 0)
                return -errno;

        n = !!tmp_dir + !!var_tmp_dir +
                strv_length(read_write_paths) +
                strv_length(read_only_paths) +
                strv_length(inaccessible_paths) +
                private_dev +
                (protect_sysctl ? 3 : 0) +
                (protect_cgroups != protect_sysctl) +
                (protect_home != PROTECT_HOME_NO ? 3 : 0) +
                (protect_system != PROTECT_SYSTEM_NO ? 2 : 0) +
                (protect_system == PROTECT_SYSTEM_FULL ? 1 : 0);

        if (n > 0) {
                m = mounts = (BindMount *) alloca0(n * sizeof(BindMount));
                r = append_mounts(&m, read_write_paths, READWRITE);
                if (r < 0)
                        return r;

                r = append_mounts(&m, read_only_paths, READONLY);
                if (r < 0)
                        return r;

                r = append_mounts(&m, inaccessible_paths, INACCESSIBLE);
                if (r < 0)
                        return r;

                if (tmp_dir) {
                        m->path = prefix_roota(root_directory, "/tmp");
                        m->mode = PRIVATE_TMP;
                        m++;
                }

                if (var_tmp_dir) {
                        m->path = prefix_roota(root_directory, "/var/tmp");
                        m->mode = PRIVATE_VAR_TMP;
                        m++;
                }

                if (private_dev) {
                        m->path = prefix_roota(root_directory, "/dev");
                        m->mode = PRIVATE_DEV;
                        m++;
                }

                if (protect_sysctl) {
                        m->path = prefix_roota(root_directory, "/proc/sys");
                        m->mode = READONLY;
                        m++;

                        m->path = prefix_roota(root_directory, "/proc/sysrq-trigger");
                        m->mode = READONLY;
                        m->ignore = true; /* Not always compiled into the kernel */
                        m++;

                        m->path = prefix_roota(root_directory, "/sys");
                        m->mode = READONLY;
                        m++;
                }

                if (protect_cgroups != protect_sysctl) {
                        m->path = prefix_roota(root_directory, "/sys/fs/cgroup");
                        m->mode = protect_cgroups ? READONLY : READWRITE;
                        m++;
                }

                if (protect_home != PROTECT_HOME_NO) {
                        const char *home_dir, *run_user_dir, *root_dir;

                        home_dir = prefix_roota(root_directory, "/home");
                        home_dir = strjoina("-", home_dir);
                        run_user_dir = prefix_roota(root_directory, "/run/user");
                        run_user_dir = strjoina("-", run_user_dir);
                        root_dir = prefix_roota(root_directory, "/root");
                        root_dir = strjoina("-", root_dir);

                        r = append_mounts(&m, STRV_MAKE(home_dir, run_user_dir, root_dir),
                                protect_home == PROTECT_HOME_READ_ONLY ? READONLY : INACCESSIBLE);
                        if (r < 0)
                                return r;
                }

                if (protect_system != PROTECT_SYSTEM_NO) {
                        const char *usr_dir, *boot_dir, *etc_dir;

                        usr_dir = prefix_roota(root_directory, "/usr");
                        boot_dir = prefix_roota(root_directory, "/boot");
                        boot_dir = strjoina("-", boot_dir);
                        etc_dir = prefix_roota(root_directory, "/etc");

                        r = append_mounts(&m, protect_system == PROTECT_SYSTEM_FULL
                                ? STRV_MAKE(usr_dir, boot_dir, etc_dir)
                                : STRV_MAKE(usr_dir, boot_dir), READONLY);
                        if (r < 0)
                                return r;
                }

                assert(mounts + n == m);

                qsort(mounts, n, sizeof(BindMount), mount_path_compare);

                drop_duplicates(mounts, &n);
                drop_inaccessible(mounts, &n);
                drop_nop(mounts, &n);
        }

        if (n > 0 || root_directory) {
                /* Remount / as SLAVE so that nothing now mounted in the namespace
                   shows up in the parent */
                if (mount(NULL, "/", NULL, MS_SLAVE|MS_REC, NULL) < 0)
                        return -errno;
        }

        if (root_directory) {
                /* Turn directory into bind mount */
                if (mount(root_directory, root_directory, NULL, MS_BIND|MS_REC, NULL) < 0)
                        return -errno;
        }

        if (n > 0) {
                for (m = mounts; m < mounts + n; ++m) {
                        r = apply_mount(m, tmp_dir, var_tmp_dir);
                        if (r < 0)
                                goto fail;
                }

                for (m = mounts; m < mounts + n; ++m) {
                        r = make_read_only(m);
                        if (r < 0)
                                goto fail;
                }
        }

        if (root_directory) {
                /* MS_MOVE does not work on MS_SHARED so the remount MS_SHARED will be done later */
                r = mount_move_root(root_directory);

                /* at this point, we cannot rollback */
                if (r < 0)
                        return r;
        }

        /* Remount / as the desired mode. Not that this will not
         * reestablish propagation from our side to the host, since
         * what's disconnected is disconnected. */
        if (mount(NULL, "/", NULL, mount_flags | MS_REC, NULL) < 0)
                /* at this point, we cannot rollback */
                return -errno;

        return 0;

fail:
        if (n > 0) {
                for (m = mounts; m < mounts + n; ++m) {
                        if (!m->done)
                                continue;

                        (void) umount2(m->path, MNT_DETACH);
                }
        }

        return r;
}

static int setup_one_tmp_dir(const char *id, const char *prefix, char **path) {
        _cleanup_free_ char *x = NULL;
        char bid[SD_ID128_STRING_MAX];
        sd_id128_t boot_id;
        int r;

        assert(id);
        assert(prefix);
        assert(path);

        /* We include the boot id in the directory so that after a
         * reboot we can easily identify obsolete directories. */

        r = sd_id128_get_boot(&boot_id);
        if (r < 0)
                return r;

        x = strjoin(prefix, "/systemd-private-", sd_id128_to_string(boot_id, bid), "-", id, "-XXXXXX", NULL);
        if (!x)
                return -ENOMEM;

        RUN_WITH_UMASK(0077)
                if (!mkdtemp(x))
                        return -errno;

        RUN_WITH_UMASK(0000) {
                char *y;

                y = strjoina(x, "/tmp");

                if (mkdir(y, 0777 | S_ISVTX) < 0)
                        return -errno;
        }

        *path = x;
        x = NULL;

        return 0;
}

int setup_tmp_dirs(const char *id, char **tmp_dir, char **var_tmp_dir) {
        char *a, *b;
        int r;

        assert(id);
        assert(tmp_dir);
        assert(var_tmp_dir);

        r = setup_one_tmp_dir(id, "/tmp", &a);
        if (r < 0)
                return r;

        r = setup_one_tmp_dir(id, "/var/tmp", &b);
        if (r < 0) {
                char *t;

                t = strjoina(a, "/tmp");
                rmdir(t);
                rmdir(a);

                free(a);
                return r;
        }

        *tmp_dir = a;
        *var_tmp_dir = b;

        return 0;
}

int setup_netns(int netns_storage_socket[2]) {
        _cleanup_close_ int netns = -1;
        int r, q;

        assert(netns_storage_socket);
        assert(netns_storage_socket[0] >= 0);
        assert(netns_storage_socket[1] >= 0);

        /* We use the passed socketpair as a storage buffer for our
         * namespace reference fd. Whatever process runs this first
         * shall create a new namespace, all others should just join
         * it. To serialize that we use a file lock on the socket
         * pair.
         *
         * It's a bit crazy, but hey, works great! */

        if (lockf(netns_storage_socket[0], F_LOCK, 0) < 0)
                return -errno;

        netns = receive_one_fd(netns_storage_socket[0], MSG_DONTWAIT);
        if (netns == -EAGAIN) {
                /* Nothing stored yet, so let's create a new namespace */

                if (unshare(CLONE_NEWNET) < 0) {
                        r = -errno;
                        goto fail;
                }

                loopback_setup();

                netns = open("/proc/self/ns/net", O_RDONLY|O_CLOEXEC|O_NOCTTY);
                if (netns < 0) {
                        r = -errno;
                        goto fail;
                }

                r = 1;

        } else if (netns < 0) {
                r = netns;
                goto fail;

        } else {
                /* Yay, found something, so let's join the namespace */
                if (setns(netns, CLONE_NEWNET) < 0) {
                        r = -errno;
                        goto fail;
                }

                r = 0;
        }

        q = send_one_fd(netns_storage_socket[1], netns, MSG_DONTWAIT);
        if (q < 0) {
                r = q;
                goto fail;
        }

fail:
        (void) lockf(netns_storage_socket[0], F_ULOCK, 0);
        return r;
}

static const char *const protect_home_table[_PROTECT_HOME_MAX] = {
        [PROTECT_HOME_NO] = "no",
        [PROTECT_HOME_YES] = "yes",
        [PROTECT_HOME_READ_ONLY] = "read-only",
};

DEFINE_STRING_TABLE_LOOKUP(protect_home, ProtectHome);

static const char *const protect_system_table[_PROTECT_SYSTEM_MAX] = {
        [PROTECT_SYSTEM_NO] = "no",
        [PROTECT_SYSTEM_YES] = "yes",
        [PROTECT_SYSTEM_FULL] = "full",
};

DEFINE_STRING_TABLE_LOOKUP(protect_system, ProtectSystem);
