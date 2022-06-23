/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <ctype.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#include "sd-device.h"

#include "alloc-util.h"
#include "chase-symlinks.h"
#include "device-util.h"
#include "device-util.h"
#include "devnum-util.h"
#include "dirent-util.h"
#include "env-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "fs-util.h"
#include "hashmap.h"
#include "id128-util.h"
#include "macro.h"
#include "missing_magic.h"
#include "netlink-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "set.h"
#include "socket-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "strxcpyx.h"
#include "user-util.h"
#include "util.h"

static sd_device *device_free(sd_device *device) {
        assert(device);

        sd_device_unref(device->parent);
        free(device->syspath);
        free(device->sysname);
        free(device->devtype);
        free(device->devname);
        free(device->subsystem);
        free(device->driver_subsystem);
        free(device->driver);
        free(device->device_id);
        free(device->properties_strv);
        free(device->properties_nulstr);

        ordered_hashmap_free(device->properties);
        ordered_hashmap_free(device->properties_db);
        hashmap_free(device->sysattr_values);
        set_free(device->sysattrs);
        set_free(device->all_tags);
        set_free(device->current_tags);
        set_free(device->devlinks);

        return mfree(device);
}

DEFINE_PUBLIC_TRIVIAL_REF_UNREF_FUNC(sd_device, sd_device, device_free);

_public_ int sd_device_new_from_syspath(sd_device **ret, const char *syspath) {
        _cleanup_(sd_device_unrefp) sd_device *device = NULL;
        int r;

        assert_return(ret, -EINVAL);
        assert_return(syspath, -EINVAL);

        r = device_new_aux(&device);
        if (r < 0)
                return r;

        r = device_set_syspath(device, syspath, /* verify= */ true);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(device);
        return 0;
}

static int device_new_from_mode_and_devnum(sd_device **ret, mode_t mode, dev_t devnum) {
        _cleanup_(sd_device_unrefp) sd_device *dev = NULL;
        _cleanup_free_ char *syspath = NULL;
        const char *t, *subsystem;
        dev_t n;
        int r;

        assert(ret);

        if (S_ISCHR(mode))
                t = "char";
        else if (S_ISBLK(mode))
                t = "block";
        else
                return -ENOTTY;

        if (major(devnum) == 0)
                return -ENODEV;

        if (asprintf(&syspath, "/sys/dev/%s/%u:%u", t, major(devnum), minor(devnum)) < 0)
                return -ENOMEM;

        r = sd_device_new_from_syspath(&dev, syspath);
        if (r < 0)
                return r;

        r = sd_device_get_devnum(dev, &n);
        if (r == -ENOENT)
                return -ENXIO;
        if (r < 0)
                return r;
        if (n != devnum)
                return -ENXIO;

        r = sd_device_get_subsystem(dev, &subsystem);
        if (r < 0 && r != -ENOENT)
                return r;
        if (r >= 0 && streq(subsystem, "block") != !!S_ISBLK(mode))
                return -ENXIO;

        *ret = TAKE_PTR(dev);
        return 0;
}

_public_ int sd_device_new_from_devnum(sd_device **ret, char type, dev_t devnum) {
        assert_return(ret, -EINVAL);
        assert_return(IN_SET(type, 'b', 'c'), -EINVAL);

        return device_new_from_mode_and_devnum(ret, type == 'b' ? S_IFBLK : S_IFCHR, devnum);
}

static int device_new_from_main_ifname(sd_device **ret, const char *ifname) {
        const char *syspath;

        assert(ret);
        assert(ifname);

        syspath = strjoina("/sys/class/net/", ifname);
        return sd_device_new_from_syspath(ret, syspath);
}

_public_ int sd_device_new_from_ifname(sd_device **ret, const char *ifname) {
        _cleanup_free_ char *main_name = NULL;
        int r;

        assert_return(ret, -EINVAL);
        assert_return(ifname, -EINVAL);

        r = parse_ifindex(ifname);
        if (r > 0)
                return sd_device_new_from_ifindex(ret, r);

        if (ifname_valid(ifname)) {
                r = device_new_from_main_ifname(ret, ifname);
                if (r >= 0)
                        return r;
        }

        r = rtnl_resolve_link_alternative_name(NULL, ifname, &main_name);
        if (r < 0)
                return r;

        return device_new_from_main_ifname(ret, main_name);
}

_public_ int sd_device_new_from_ifindex(sd_device **ret, int ifindex) {
        _cleanup_(sd_device_unrefp) sd_device *dev = NULL;
        char ifname[IF_NAMESIZE];
        int r, i;

        assert_return(ret, -EINVAL);
        assert_return(ifindex > 0, -EINVAL);

        if (format_ifname(ifindex, ifname) < 0)
                return -ENODEV;

        r = device_new_from_main_ifname(&dev, ifname);
        if (r < 0)
                return r;

        r = sd_device_get_ifindex(dev, &i);
        if (r == -ENOENT)
                return -ENXIO;
        if (r < 0)
                return r;
        if (i != ifindex)
                return -ENXIO;

        *ret = TAKE_PTR(dev);
        return 0;
}

static int device_strjoin_new(
                const char *a,
                const char *b,
                const char *c,
                const char *d,
                sd_device **ret) {

        const char *p;
        int r;

        p = strjoina(a, b, c, d);
        if (access(p, F_OK) < 0)
                return IN_SET(errno, ENOENT, ENAMETOOLONG) ? 0 : -errno; /* If this sysfs is too long then it doesn't exist either */

        r = sd_device_new_from_syspath(ret, p);
        if (r < 0)
                return r;

        return 1;
}

_public_ int sd_device_new_from_subsystem_sysname(
                sd_device **ret,
                const char *subsystem,
                const char *sysname) {

        char *name;
        int r;

        assert_return(ret, -EINVAL);
        assert_return(path_is_normalized(subsystem), -EINVAL);
        assert_return(path_is_normalized(sysname), -EINVAL);

        /* translate sysname back to sysfs filename */
        name = strdupa_safe(sysname);
        string_replace_char(name, '/', '!');

        if (streq(subsystem, "subsystem")) {
                FOREACH_STRING(s, "/sys/bus/", "/sys/class/") {
                        r = device_strjoin_new(s, name, NULL, NULL, ret);
                        if (r < 0)
                                return r;
                        if (r > 0)
                                return 0;
                }

        } else if (streq(subsystem, "module")) {
                r = device_strjoin_new("/sys/module/", name, NULL, NULL, ret);
                if (r < 0)
                        return r;
                if (r > 0)
                        return 0;

        } else if (streq(subsystem, "drivers")) {
                const char *sep;

                sep = strchr(name, ':');
                if (sep && sep[1] != '\0') { /* Require ":" and something non-empty after that. */

                        const char *subsys = memdupa_suffix0(name, sep - name);
                        sep++;

                        if (streq(sep, "drivers")) /* If the sysname is "drivers", then it's the drivers directory itself that is meant. */
                                r = device_strjoin_new("/sys/bus/", subsys, "/drivers", NULL, ret);
                        else
                                r = device_strjoin_new("/sys/bus/", subsys, "/drivers/", sep, ret);
                        if (r < 0)
                                return r;
                        if (r > 0)
                                return 0;
                }
        }

        r = device_strjoin_new("/sys/bus/", subsystem, "/devices/", name, ret);
        if (r < 0)
                return r;
        if (r > 0)
                return 0;

        r = device_strjoin_new("/sys/class/", subsystem, "/", name, ret);
        if (r < 0)
                return r;
        if (r > 0)
                return 0;

        r = device_strjoin_new("/sys/firmware/", subsystem, "/", name, ret);
        if (r < 0)
                return r;
        if (r > 0)
                return 0;

        return -ENODEV;
}

_public_ int sd_device_new_from_stat_rdev(sd_device **ret, const struct stat *st) {
        assert_return(ret, -EINVAL);
        assert_return(st, -EINVAL);

        return device_new_from_mode_and_devnum(ret, st->st_mode, st->st_rdev);
}

_public_ int sd_device_new_from_devname(sd_device **ret, const char *devname) {
        struct stat st;
        dev_t devnum;
        mode_t mode;

        assert_return(ret, -EINVAL);
        assert_return(devname, -EINVAL);

        /* This function actually accepts both devlinks and devnames, i.e. both symlinks and device
         * nodes below /dev/. */

        /* Also ignore when the specified path is "/dev". */
        if (isempty(path_startswith(devname, "/dev")))
                return -EINVAL;

        if (device_path_parse_major_minor(devname, &mode, &devnum) >= 0)
                /* Let's shortcut when "/dev/block/maj:min" or "/dev/char/maj:min" is specified.
                 * In that case, we can directly convert the path to syspath, hence it is not necessary
                 * that the specified path exists. So, this works fine without udevd being running. */
                return device_new_from_mode_and_devnum(ret, mode, devnum);

        if (stat(devname, &st) < 0)
                return ERRNO_IS_DEVICE_ABSENT(errno) ? -ENODEV : -errno;

        return sd_device_new_from_stat_rdev(ret, &st);
}

_public_ int sd_device_new_from_path(sd_device **ret, const char *path) {
        assert_return(ret, -EINVAL);
        assert_return(path, -EINVAL);

        if (path_startswith(path, "/dev"))
                return sd_device_new_from_devname(ret, path);

        return sd_device_new_from_syspath(ret, path);
}

_public_ int sd_device_get_ifindex(sd_device *device, int *ifindex) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        if (device->ifindex <= 0)
                return -ENOENT;

        if (ifindex)
                *ifindex = device->ifindex;

        return 0;
}

_public_ int sd_device_new_from_device_id(sd_device **ret, const char *id) {
        int r;

        assert_return(ret, -EINVAL);
        assert_return(id, -EINVAL);

        switch (id[0]) {
        case 'b':
        case 'c': {
                dev_t devt;

                if (isempty(id))
                        return -EINVAL;

                r = parse_devnum(id + 1, &devt);
                if (r < 0)
                        return r;

                return sd_device_new_from_devnum(ret, id[0], devt);
        }

        case 'n': {
                int ifindex;

                ifindex = parse_ifindex(id + 1);
                if (ifindex < 0)
                        return ifindex;

                return sd_device_new_from_ifindex(ret, ifindex);
        }

        case '+': {
                const char *subsys, *sep;

                sep = strchr(id + 1, ':');
                if (!sep || sep - id - 1 > NAME_MAX)
                        return -EINVAL;

                subsys = memdupa_suffix0(id + 1, sep - id - 1);

                return sd_device_new_from_subsystem_sysname(ret, subsys, sep + 1);
        }

        default:
                return -EINVAL;
        }
}

_public_ int sd_device_get_syspath(sd_device *device, const char **ret) {
        assert_return(device, -EINVAL);

        assert(path_startswith(device->syspath, "/sys/"));

        if (ret)
                *ret = device->syspath;

        return 0;
}

static int device_new_from_child(sd_device **ret, sd_device *child) {
        _cleanup_free_ char *path = NULL;
        const char *syspath;
        int r;

        assert(ret);
        assert(child);

        r = sd_device_get_syspath(child, &syspath);
        if (r < 0)
                return r;

        for (;;) {
                _cleanup_free_ char *p = NULL;

                r = path_extract_directory(path ?: syspath, &p);
                if (r < 0)
                        return r;

                if (path_equal(p, "/sys"))
                        return -ENODEV;

                r = sd_device_new_from_syspath(ret, p);
                if (r != -ENODEV)
                        return r;

                free_and_replace(path, p);
        }
}

_public_ int sd_device_get_parent(sd_device *child, sd_device **ret) {
        int r;

        assert_return(child, -EINVAL);

        if (!child->parent_set) {
                r = device_new_from_child(&child->parent, child);
                if (r < 0 && r != -ENODEV)
                        return r;

                child->parent_set = true;
        }

        if (!child->parent)
                return -ENOENT;

        if (ret)
                *ret = child->parent;
        return 0;
}

_public_ int sd_device_get_subsystem(sd_device *device, const char **ret) {
        int r;

        assert_return(device, -EINVAL);

        if (!device->subsystem_set) {
                _cleanup_free_ char *subsystem = NULL;
                const char *syspath;
                char *path;

                r = sd_device_get_syspath(device, &syspath);
                if (r < 0)
                        return r;

                /* read 'subsystem' link */
                path = strjoina(syspath, "/subsystem");
                r = readlink_value(path, &subsystem);
                if (r < 0 && r != -ENOENT)
                        return log_device_debug_errno(device, r,
                                                      "sd-device: Failed to read subsystem for %s: %m",
                                                      device->devpath);

                if (subsystem)
                        r = device_set_subsystem(device, subsystem);
                /* use implicit names */
                else if (!isempty(path_startswith(device->devpath, "/module/")))
                        r = device_set_subsystem(device, "module");
                else if (strstr(syspath, "/drivers/") || endswith(syspath, "/drivers"))
                        r = device_set_drivers_subsystem(device);
                else if (!isempty(PATH_STARTSWITH_SET(device->devpath, "/class/", "/bus/")))
                        r = device_set_subsystem(device, "subsystem");
                else {
                        device->subsystem_set = true;
                        r = 0;
                }
                if (r < 0)
                        return log_device_debug_errno(device, r,
                                                      "sd-device: Failed to set subsystem for %s: %m",
                                                      device->devpath);
        }

        if (!device->subsystem)
                return -ENOENT;

        if (ret)
                *ret = device->subsystem;
        return 0;
}

_public_ int sd_device_get_devtype(sd_device *device, const char **devtype) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        if (!device->devtype)
                return -ENOENT;

        if (devtype)
                *devtype = device->devtype;

        return !!device->devtype;
}

_public_ int sd_device_get_parent_with_subsystem_devtype(sd_device *child, const char *subsystem, const char *devtype, sd_device **ret) {
        sd_device *parent = NULL;
        int r;

        assert_return(child, -EINVAL);
        assert_return(subsystem, -EINVAL);

        r = sd_device_get_parent(child, &parent);
        while (r >= 0) {
                const char *parent_subsystem = NULL;

                (void) sd_device_get_subsystem(parent, &parent_subsystem);
                if (streq_ptr(parent_subsystem, subsystem)) {
                        const char *parent_devtype = NULL;

                        if (!devtype)
                                break;

                        (void) sd_device_get_devtype(parent, &parent_devtype);
                        if (streq_ptr(parent_devtype, devtype))
                                break;
                }
                r = sd_device_get_parent(parent, &parent);
        }

        if (r < 0)
                return r;

        if (ret)
                *ret = parent;
        return 0;
}

_public_ int sd_device_get_devnum(sd_device *device, dev_t *devnum) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        if (major(device->devnum) <= 0)
                return -ENOENT;

        if (devnum)
                *devnum = device->devnum;

        return 0;
}

_public_ int sd_device_get_driver(sd_device *device, const char **ret) {
        assert_return(device, -EINVAL);

        if (!device->driver_set) {
                _cleanup_free_ char *driver = NULL;
                const char *syspath;
                char *path;
                int r;

                r = sd_device_get_syspath(device, &syspath);
                if (r < 0)
                        return r;

                path = strjoina(syspath, "/driver");
                r = readlink_value(path, &driver);
                if (r < 0 && r != -ENOENT)
                        return log_device_debug_errno(device, r,
                                                      "sd-device: readlink(\"%s\") failed: %m", path);

                r = device_set_driver(device, driver);
                if (r < 0)
                        return log_device_debug_errno(device, r,
                                                      "sd-device: Failed to set driver \"%s\": %m", driver);
        }

        if (!device->driver)
                return -ENOENT;

        if (ret)
                *ret = device->driver;
        return 0;
}

_public_ int sd_device_get_devpath(sd_device *device, const char **ret) {
        assert_return(device, -EINVAL);

        assert(device->devpath);
        assert(device->devpath[0] == '/');

        if (ret)
                *ret = device->devpath;

        return 0;
}

_public_ int sd_device_get_devname(sd_device *device, const char **devname) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        if (!device->devname)
                return -ENOENT;

        assert(path_startswith(device->devname, "/dev/"));

        if (devname)
                *devname = device->devname;
        return 0;
}

static int device_set_sysname_and_sysnum(sd_device *device) {
        _cleanup_free_ char *sysname = NULL;
        size_t len, n;
        int r;

        assert(device);

        r = path_extract_filename(device->devpath, &sysname);
        if (r < 0)
                return r;
        if (r == O_DIRECTORY)
                return -EINVAL;

        /* some devices have '!' in their name, change that to '/' */
        string_replace_char(sysname, '!', '/');

        n = strspn_from_end(sysname, DIGITS);
        len = strlen(sysname);
        assert(n <= len);
        if (n == len)
                n = 0; /* Do not set sysnum for number only sysname. */

        device->sysnum = n > 0 ? sysname + len - n : NULL;
        return free_and_replace(device->sysname, sysname);
}

_public_ int sd_device_get_sysname(sd_device *device, const char **ret) {
        int r;

        assert_return(device, -EINVAL);

        if (!device->sysname) {
                r = device_set_sysname_and_sysnum(device);
                if (r < 0)
                        return r;
        }

        if (ret)
                *ret = device->sysname;
        return 0;
}

_public_ int sd_device_get_sysnum(sd_device *device, const char **ret) {
        int r;

        assert_return(device, -EINVAL);

        if (!device->sysname) {
                r = device_set_sysname_and_sysnum(device);
                if (r < 0)
                        return r;
        }

        if (!device->sysnum)
                return -ENOENT;

        if (ret)
                *ret = device->sysnum;
        return 0;
}

_public_ int sd_device_get_action(sd_device *device, sd_device_action_t *ret) {
        assert_return(device, -EINVAL);

        if (device->action < 0)
                return -ENOENT;

        if (ret)
                *ret = device->action;

        return 0;
}

_public_ int sd_device_get_seqnum(sd_device *device, uint64_t *ret) {
        assert_return(device, -EINVAL);

        if (device->seqnum == 0)
                return -ENOENT;

        if (ret)
                *ret = device->seqnum;

        return 0;
}

_public_ int sd_device_get_diskseq(sd_device *device, uint64_t *ret) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_uevent_file(device);
        if (r < 0)
                return r;

        if (device->diskseq == 0)
                return -ENOENT;

        if (ret)
                *ret = device->diskseq;

        return 0;
}

_public_ int sd_device_get_is_initialized(sd_device *device) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_db(device);
        if (r < 0)
                return r;

        return device->is_initialized;
}

_public_ int sd_device_get_usec_initialized(sd_device *device, uint64_t *ret) {
        int r;

        assert_return(device, -EINVAL);

        r = device_read_db(device);
        if (r < 0)
                return r;

        if (!device->is_initialized)
                return -EBUSY;

        if (device->usec_initialized == 0)
                return -ENODATA;

        if (ret)
                *ret = device->usec_initialized;

        return 0;
}

_public_ int sd_device_get_usec_since_initialized(sd_device *device, uint64_t *usec) {
        usec_t now_ts;
        int r;

        assert_return(device, -EINVAL);

        r = device_read_db(device);
        if (r < 0)
                return r;

        if (!device->is_initialized)
                return -EBUSY;

        if (device->usec_initialized == 0)
                return -ENODATA;

        now_ts = now(CLOCK_MONOTONIC);

        if (now_ts < device->usec_initialized)
                return -EIO;

        if (usec)
                *usec = now_ts - device->usec_initialized;
        return 0;
}

_public_ const char *sd_device_get_tag_first(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        (void) device_read_db(device);

        device->all_tags_iterator_generation = device->tags_generation;
        device->all_tags_iterator = ITERATOR_FIRST;

        (void) set_iterate(device->all_tags, &device->all_tags_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_tag_next(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        (void) device_read_db(device);

        if (device->all_tags_iterator_generation != device->tags_generation)
                return NULL;

        (void) set_iterate(device->all_tags, &device->all_tags_iterator, &v);
        return v;
}

static bool device_database_supports_current_tags(sd_device *device) {
        assert(device);

        (void) device_read_db(device);

        /* The current tags (saved in Q field) feature is implemented in database version 1.
         * If the database version is 0, then the tags (NOT current tags, saved in G field) are not
         * sticky. Thus, we can safely bypass the operations for the current tags (Q) to tags (G). */

        return device->database_version >= 1;
}

_public_ const char *sd_device_get_current_tag_first(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        if (!device_database_supports_current_tags(device))
                return sd_device_get_tag_first(device);

        (void) device_read_db(device);

        device->current_tags_iterator_generation = device->tags_generation;
        device->current_tags_iterator = ITERATOR_FIRST;

        (void) set_iterate(device->current_tags, &device->current_tags_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_current_tag_next(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        if (!device_database_supports_current_tags(device))
                return sd_device_get_tag_next(device);

        (void) device_read_db(device);

        if (device->current_tags_iterator_generation != device->tags_generation)
                return NULL;

        (void) set_iterate(device->current_tags, &device->current_tags_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_devlink_first(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        (void) device_read_db(device);

        device->devlinks_iterator_generation = device->devlinks_generation;
        device->devlinks_iterator = ITERATOR_FIRST;

        (void) set_iterate(device->devlinks, &device->devlinks_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_devlink_next(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        (void) device_read_db(device);

        if (device->devlinks_iterator_generation != device->devlinks_generation)
                return NULL;

        (void) set_iterate(device->devlinks, &device->devlinks_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_property_first(sd_device *device, const char **_value) {
        const char *key;
        int r;

        assert_return(device, NULL);

        r = device_properties_prepare(device);
        if (r < 0)
                return NULL;

        device->properties_iterator_generation = device->properties_generation;
        device->properties_iterator = ITERATOR_FIRST;

        (void) ordered_hashmap_iterate(device->properties, &device->properties_iterator, (void**)_value, (const void**)&key);
        return key;
}

_public_ const char *sd_device_get_property_next(sd_device *device, const char **_value) {
        const char *key;
        int r;

        assert_return(device, NULL);

        r = device_properties_prepare(device);
        if (r < 0)
                return NULL;

        if (device->properties_iterator_generation != device->properties_generation)
                return NULL;

        (void) ordered_hashmap_iterate(device->properties, &device->properties_iterator, (void**)_value, (const void**)&key);
        return key;
}

static int device_sysattrs_read_all_internal(sd_device *device, const char *subdir) {
        _cleanup_free_ char *path_dir = NULL;
        _cleanup_closedir_ DIR *dir = NULL;
        const char *syspath;
        int r;

        r = sd_device_get_syspath(device, &syspath);
        if (r < 0)
                return r;

        if (subdir) {
                _cleanup_free_ char *p = NULL;

                p = path_join(syspath, subdir, "uevent");
                if (!p)
                        return -ENOMEM;

                if (access(p, F_OK) >= 0)
                        /* this is a child device, skipping */
                        return 0;
                if (errno != ENOENT) {
                        log_device_debug_errno(device, errno, "sd-device: Failed to stat %s, ignoring subdir: %m", p);
                        return 0;
                }

                path_dir = path_join(syspath, subdir);
                if (!path_dir)
                        return -ENOMEM;
        }

        dir = opendir(path_dir ?: syspath);
        if (!dir)
                return -errno;

        FOREACH_DIRENT_ALL(de, dir, return -errno) {
                _cleanup_free_ char *path = NULL, *p = NULL;
                struct stat statbuf;

                if (dot_or_dot_dot(de->d_name))
                        continue;

                /* only handle symlinks, regular files, and directories */
                if (!IN_SET(de->d_type, DT_LNK, DT_REG, DT_DIR))
                        continue;

                if (subdir) {
                        p = path_join(subdir, de->d_name);
                        if (!p)
                                return -ENOMEM;
                }

                if (de->d_type == DT_DIR) {
                        /* read subdirectory */
                        r = device_sysattrs_read_all_internal(device, p ?: de->d_name);
                        if (r < 0)
                                return r;

                        continue;
                }

                path = path_join(syspath, p ?: de->d_name);
                if (!path)
                        return -ENOMEM;

                if (lstat(path, &statbuf) != 0)
                        continue;

                if ((statbuf.st_mode & (S_IRUSR | S_IWUSR)) == 0)
                        continue;

                r = set_put_strdup(&device->sysattrs, p ?: de->d_name);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int device_sysattrs_read_all(sd_device *device) {
        int r;

        assert(device);

        if (device->sysattrs_read)
                return 0;

        r = device_sysattrs_read_all_internal(device, NULL);
        if (r < 0)
                return r;

        device->sysattrs_read = true;

        return 0;
}

_public_ const char *sd_device_get_sysattr_first(sd_device *device) {
        void *v;
        int r;

        assert_return(device, NULL);

        if (!device->sysattrs_read) {
                r = device_sysattrs_read_all(device);
                if (r < 0) {
                        errno = -r;
                        return NULL;
                }
        }

        device->sysattrs_iterator = ITERATOR_FIRST;

        (void) set_iterate(device->sysattrs, &device->sysattrs_iterator, &v);
        return v;
}

_public_ const char *sd_device_get_sysattr_next(sd_device *device) {
        void *v;

        assert_return(device, NULL);

        if (!device->sysattrs_read)
                return NULL;

        (void) set_iterate(device->sysattrs, &device->sysattrs_iterator, &v);
        return v;
}

_public_ int sd_device_has_tag(sd_device *device, const char *tag) {
        assert_return(device, -EINVAL);
        assert_return(tag, -EINVAL);

        (void) device_read_db(device);

        return set_contains(device->all_tags, tag);
}

_public_ int sd_device_has_current_tag(sd_device *device, const char *tag) {
        assert_return(device, -EINVAL);
        assert_return(tag, -EINVAL);

        if (!device_database_supports_current_tags(device))
                return sd_device_has_tag(device, tag);

        (void) device_read_db(device);

        return set_contains(device->current_tags, tag);
}

_public_ int sd_device_get_property_value(sd_device *device, const char *key, const char **ret_value) {
        const char *value;
        int r;

        assert_return(device, -EINVAL);
        assert_return(key, -EINVAL);

        r = device_properties_prepare(device);
        if (r < 0)
                return r;

        value = ordered_hashmap_get(device->properties, key);
        if (!value)
                return -ENOENT;

        if (ret_value)
                *ret_value = value;
        return 0;
}

_public_ int sd_device_get_trigger_uuid(sd_device *device, sd_id128_t *ret) {
        const char *s;
        sd_id128_t id;
        int r;

        assert_return(device, -EINVAL);

        /* Retrieves the UUID attached to a uevent when triggering it from userspace via
         * sd_device_trigger_with_uuid() or an equivalent interface. Returns -ENOENT if the record is not
         * caused by a synthetic event and -ENODATA if it was but no UUID was specified */

        r = sd_device_get_property_value(device, "SYNTH_UUID", &s);
        if (r < 0)
                return r;

        if (streq(s, "0")) /* SYNTH_UUID=0 is set whenever a device is triggered by userspace without specifying a UUID */
                return -ENODATA;

        r = sd_id128_from_string(s, &id);
        if (r < 0)
                return r;

        if (ret)
                *ret = id;

        return 0;
}

/* We cache all sysattr lookups. If an attribute does not exist, it is stored
 * with a NULL value in the cache, otherwise the returned string is stored */
_public_ int sd_device_get_sysattr_value(sd_device *device, const char *sysattr, const char **ret_value) {
        _cleanup_free_ char *value = NULL;
        const char *path, *syspath;
        struct stat statbuf;
        int r;

        assert_return(device, -EINVAL);
        assert_return(sysattr, -EINVAL);

        /* look for possibly already cached result */
        r = device_get_cached_sysattr_value(device, sysattr, ret_value);
        if (r != -ESTALE)
                return r;

        r = sd_device_get_syspath(device, &syspath);
        if (r < 0)
                return r;

        path = prefix_roota(syspath, sysattr);
        if (lstat(path, &statbuf) < 0) {
                int k;

                r = -errno;

                /* remember that we could not access the sysattr */
                k = device_cache_sysattr_value(device, sysattr, NULL);
                if (k < 0)
                        log_device_debug_errno(device, k,
                                               "sd-device: failed to cache attribute '%s' with NULL, ignoring: %m",
                                               sysattr);

                return r;
        } else if (S_ISLNK(statbuf.st_mode)) {
                /* Some core links return only the last element of the target path,
                 * these are just values, the paths should not be exposed. */
                if (STR_IN_SET(sysattr, "driver", "subsystem", "module")) {
                        r = readlink_value(path, &value);
                        if (r < 0)
                                return r;
                } else
                        return -EINVAL;
        } else if (S_ISDIR(statbuf.st_mode))
                /* skip directories */
                return -EISDIR;
        else if (!(statbuf.st_mode & S_IRUSR))
                /* skip non-readable files */
                return -EPERM;
        else {
                size_t size;

                /* Read attribute value, Some attributes contain embedded '\0'. So, it is necessary to
                 * also get the size of the result. See issue #20025. */
                r = read_full_virtual_file(path, &value, &size);
                if (r < 0)
                        return r;

                /* drop trailing newlines */
                while (size > 0 && strchr(NEWLINE, value[--size]))
                        value[size] = '\0';
        }

        /* Unfortunately, we need to return 'const char*' instead of 'char*'. Hence, failure in caching
         * sysattr value is critical unlike the other places. */
        r = device_cache_sysattr_value(device, sysattr, value);
        if (r < 0) {
                log_device_debug_errno(device, r,
                                       "sd-device: failed to cache attribute '%s' with '%s'%s: %m",
                                       sysattr, value, ret_value ? "" : ", ignoring");
                if (ret_value)
                        return r;
        } else if (ret_value)
                *ret_value = TAKE_PTR(value);

        return 0;
}

static void device_remove_cached_sysattr_value(sd_device *device, const char *_key) {
        _cleanup_free_ char *key = NULL;

        assert(device);
        assert(_key);

        free(hashmap_remove2(device->sysattr_values, _key, (void **) &key));
}

_public_ int sd_device_set_sysattr_value(sd_device *device, const char *sysattr, const char *_value) {
        _cleanup_free_ char *value = NULL;
        const char *syspath, *path;
        size_t len;
        int r;

        assert_return(device, -EINVAL);
        assert_return(sysattr, -EINVAL);

        /* Set the attribute and save it in the cache. */

        if (!_value) {
                /* If input value is NULL, then clear cache and not write anything. */
                device_remove_cached_sysattr_value(device, sysattr);
                return 0;
        }

        r = sd_device_get_syspath(device, &syspath);
        if (r < 0)
                return r;

        path = prefix_roota(syspath, sysattr);

        len = strlen(_value);

        /* drop trailing newlines */
        while (len > 0 && strchr(NEWLINE, _value[len - 1]))
                len --;

        /* value length is limited to 4k */
        if (len > 4096)
                return -EINVAL;

        value = strndup(_value, len);
        if (!value)
                return -ENOMEM;

        r = write_string_file(path, value, WRITE_STRING_FILE_DISABLE_BUFFER | WRITE_STRING_FILE_NOFOLLOW);
        if (r < 0) {
                /* On failure, clear cache entry, as we do not know how it fails. */
                device_remove_cached_sysattr_value(device, sysattr);
                return r;
        }

        /* Do not cache action string written into uevent file. */
        if (streq(sysattr, "uevent"))
                return 0;

        r = device_cache_sysattr_value(device, sysattr, value);
        if (r < 0)
                log_device_debug_errno(device, r,
                                       "sd-device: failed to cache attribute '%s' with '%s', ignoring: %m",
                                       sysattr, value);
        else
                TAKE_PTR(value);

        return 0;
}

_public_ int sd_device_set_sysattr_valuef(sd_device *device, const char *sysattr, const char *format, ...) {
        _cleanup_free_ char *value = NULL;
        va_list ap;
        int r;

        assert_return(device, -EINVAL);
        assert_return(sysattr, -EINVAL);

        if (!format) {
                device_remove_cached_sysattr_value(device, sysattr);
                return 0;
        }

        va_start(ap, format);
        r = vasprintf(&value, format, ap);
        va_end(ap);

        if (r < 0)
                return -ENOMEM;

        return sd_device_set_sysattr_value(device, sysattr, value);
}

_public_ int sd_device_trigger(sd_device *device, sd_device_action_t action) {
        const char *s;

        assert_return(device, -EINVAL);

        s = device_action_to_string(action);
        if (!s)
                return -EINVAL;

        /* This uses the simple no-UUID interface of kernel < 4.13 */
        return sd_device_set_sysattr_value(device, "uevent", s);
}

_public_ int sd_device_trigger_with_uuid(
                sd_device *device,
                sd_device_action_t action,
                sd_id128_t *ret_uuid) {

        const char *s, *j;
        sd_id128_t u;
        int r;

        assert_return(device, -EINVAL);

        /* If no one wants to know the UUID, use the simple interface from pre-4.13 times */
        if (!ret_uuid)
                return sd_device_trigger(device, action);

        s = device_action_to_string(action);
        if (!s)
                return -EINVAL;

        r = sd_id128_randomize(&u);
        if (r < 0)
                return r;

        j = strjoina(s, " ", SD_ID128_TO_UUID_STRING(u));

        r = sd_device_set_sysattr_value(device, "uevent", j);
        if (r < 0)
                return r;

        *ret_uuid = u;
        return 0;
}

_public_ int sd_device_open(sd_device *device, int flags) {
        _cleanup_close_ int fd = -1, fd2 = -1;
        const char *devname, *subsystem = NULL;
        uint64_t q, diskseq = 0;
        struct stat st;
        dev_t devnum;
        int r;

        assert_return(device, -EINVAL);
        assert_return(FLAGS_SET(flags, O_PATH) || !FLAGS_SET(flags, O_NOFOLLOW), -EINVAL);

        r = sd_device_get_devname(device, &devname);
        if (r == -ENOENT)
                return -ENOEXEC;
        if (r < 0)
                return r;

        r = sd_device_get_devnum(device, &devnum);
        if (r == -ENOENT)
                return -ENOEXEC;
        if (r < 0)
                return r;

        r = sd_device_get_subsystem(device, &subsystem);
        if (r < 0 && r != -ENOENT)
                return r;

        fd = open(devname, FLAGS_SET(flags, O_PATH) ? flags : O_CLOEXEC|O_NOFOLLOW|O_PATH);
        if (fd < 0)
                return -errno;

        if (fstat(fd, &st) < 0)
                return -errno;

        if (st.st_rdev != devnum)
                return -ENXIO;

        if (streq_ptr(subsystem, "block") ? !S_ISBLK(st.st_mode) : !S_ISCHR(st.st_mode))
                return -ENXIO;

        /* If flags has O_PATH, then we cannot check diskseq. Let's return earlier. */
        if (FLAGS_SET(flags, O_PATH))
                return TAKE_FD(fd);

        r = device_get_property_bool(device, "ID_IGNORE_DISKSEQ");
        if (r < 0 && r != -ENOENT)
                return r;
        if (r <= 0) {
                r = sd_device_get_diskseq(device, &diskseq);
                if (r < 0 && r != -ENOENT)
                        return r;
        }

        fd2 = open(FORMAT_PROC_FD_PATH(fd), flags);
        if (fd2 < 0)
                return -errno;

        if (diskseq == 0)
                return TAKE_FD(fd2);

        r = fd_get_diskseq(fd2, &q);
        if (r < 0)
                return r;

        if (q != diskseq)
                return -ENXIO;

        return TAKE_FD(fd2);
}
