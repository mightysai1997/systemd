/*
 * probe disks for filesystems and partitions
 *
 * Copyright (C) 2011 Kay Sievers <kay@vrfy.org>
 * Copyright (C) 2011 Karel Zak <kzak@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <blkid/blkid.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "sd-id128.h"

#include "alloc-util.h"
#include "efivars.h"
#include "fd-util.h"
#include "gpt.h"
#include "string-util.h"
#include "udev.h"

static void print_property(struct udev_device *dev, bool test, const char *name, const char *value) {
        char s[256];

        s[0] = '\0';

        if (streq(name, "TYPE")) {
                udev_builtin_add_property(dev, test, "ID_FS_TYPE", value);

        } else if (streq(name, "USAGE")) {
                udev_builtin_add_property(dev, test, "ID_FS_USAGE", value);

        } else if (streq(name, "VERSION")) {
                udev_builtin_add_property(dev, test, "ID_FS_VERSION", value);

        } else if (streq(name, "UUID")) {
                blkid_safe_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_UUID", s);
                blkid_encode_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_UUID_ENC", s);

        } else if (streq(name, "UUID_SUB")) {
                blkid_safe_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_UUID_SUB", s);
                blkid_encode_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_UUID_SUB_ENC", s);

        } else if (streq(name, "LABEL")) {
                blkid_safe_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_LABEL", s);
                blkid_encode_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_LABEL_ENC", s);

        } else if (streq(name, "PTTYPE")) {
                udev_builtin_add_property(dev, test, "ID_PART_TABLE_TYPE", value);

        } else if (streq(name, "PTUUID")) {
                udev_builtin_add_property(dev, test, "ID_PART_TABLE_UUID", value);

        } else if (streq(name, "PART_ENTRY_NAME")) {
                blkid_encode_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_PART_ENTRY_NAME", s);

        } else if (streq(name, "PART_ENTRY_TYPE")) {
                blkid_encode_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_PART_ENTRY_TYPE", s);

        } else if (startswith(name, "PART_ENTRY_")) {
                strscpyl(s, sizeof(s), "ID_", name, NULL);
                udev_builtin_add_property(dev, test, s, value);

        } else if (streq(name, "SYSTEM_ID")) {
                blkid_encode_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_SYSTEM_ID", s);

        } else if (streq(name, "PUBLISHER_ID")) {
                blkid_encode_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_PUBLISHER_ID", s);

        } else if (streq(name, "APPLICATION_ID")) {
                blkid_encode_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_APPLICATION_ID", s);

        } else if (streq(name, "BOOT_SYSTEM_ID")) {
                blkid_encode_string(value, s, sizeof(s));
                udev_builtin_add_property(dev, test, "ID_FS_BOOT_SYSTEM_ID", s);
        }
}

static int find_gpt_root(struct udev_device *dev, blkid_probe pr, bool test) {

#if defined(GPT_ROOT_NATIVE) && defined(ENABLE_EFI)

        _cleanup_free_ char *root_id = NULL;
        bool found_esp = false;
        blkid_partlist pl;
        int i, nvals, r;

        assert(pr);

        /* Iterate through the partitions on this disk, and see if the
         * EFI ESP we booted from is on it. If so, find the first root
         * disk, and add a property indicating its partition UUID. */

        errno = 0;
        pl = blkid_probe_get_partitions(pr);
        if (!pl)
                return errno > 0 ? -errno : -ENOMEM;

        nvals = blkid_partlist_numof_partitions(pl);
        for (i = 0; i < nvals; i++) {
                blkid_partition pp;
                const char *stype, *sid;
                sd_id128_t type;

                pp = blkid_partlist_get_partition(pl, i);
                if (!pp)
                        continue;

                sid = blkid_partition_get_uuid(pp);
                if (!sid)
                        continue;

                stype = blkid_partition_get_type_string(pp);
                if (!stype)
                        continue;

                if (sd_id128_from_string(stype, &type) < 0)
                        continue;

                if (sd_id128_equal(type, GPT_ESP)) {
                        sd_id128_t id, esp;

                        /* We found an ESP, let's see if it matches
                         * the ESP we booted from. */

                        if (sd_id128_from_string(sid, &id) < 0)
                                continue;

                        r = efi_loader_get_device_part_uuid(&esp);
                        if (r < 0)
                                return r;

                        if (sd_id128_equal(id, esp))
                                found_esp = true;

                } else if (sd_id128_equal(type, GPT_ROOT_NATIVE)) {
                        unsigned long long flags;

                        flags = blkid_partition_get_flags(pp);
                        if (flags & GPT_FLAG_NO_AUTO)
                                continue;

                        /* We found a suitable root partition, let's
                         * remember the first one. */

                        if (!root_id) {
                                root_id = strdup(sid);
                                if (!root_id)
                                        return -ENOMEM;
                        }
                }
        }

        /* We found the ESP on this disk, and also found a root
         * partition, nice! Let's export its UUID */
        if (found_esp && root_id)
                udev_builtin_add_property(dev, test, "ID_PART_GPT_AUTO_ROOT_UUID", root_id);
#endif

        return 0;
}

static int probe_superblocks(blkid_probe pr) {
        struct stat st;
        int rc;

        if (fstat(blkid_probe_get_fd(pr), &st))
                return -1;

        blkid_probe_enable_partitions(pr, 1);

        if (!S_ISCHR(st.st_mode) &&
            blkid_probe_get_size(pr) <= 1024 * 1440 &&
            blkid_probe_is_wholedisk(pr)) {
                /*
                 * check if the small disk is partitioned, if yes then
                 * don't probe for filesystems.
                 */
                blkid_probe_enable_superblocks(pr, 0);

                rc = blkid_do_fullprobe(pr);
                if (rc < 0)
                        return rc;        /* -1 = error, 1 = nothing, 0 = success */

                if (blkid_probe_lookup_value(pr, "PTTYPE", NULL, NULL) == 0)
                        return 0;        /* partition table detected */
        }

        blkid_probe_set_partitions_flags(pr, BLKID_PARTS_ENTRY_DETAILS);
        blkid_probe_enable_superblocks(pr, 1);

        return blkid_do_safeprobe(pr);
}

static int builtin_blkid(struct udev_device *dev, int argc, char *argv[], bool test) {
        const char *root_partition;
        int64_t offset = 0;
        bool noraid = false;
        _cleanup_close_ int fd = -1;
        blkid_probe pr;
        const char *data;
        const char *name;
        const char *prtype = NULL;
        int nvals;
        int i;
        int err = 0;
        bool is_gpt = false;
        char** superblock_filter_list = NULL;
        char** partition_filter_list = NULL;
        int superblock_filter_list_length = 0;
        int partition_filter_list_length = 0;

        static const struct option options[] = {
                { "offset", optional_argument, NULL, 'o' },
                { "noraid", no_argument, NULL, 'R' },
                { "filter-superblock-type", required_argument, NULL, 's' },
                { "filter-partition-type", required_argument, NULL, 'p' },
                {}
        };

        for (;;) {
                int option;

                option = getopt_long(argc, argv, "oRs:p:", options, NULL);
                if (option == -1)
                        break;

                switch (option) {
                case 'o':
                        offset = strtoull(optarg, NULL, 0);
                        break;
                case 'R':
                        noraid = true;
                        break;
                case 's':
                        superblock_filter_list_length++;
                        superblock_filter_list = (char**) realloc(superblock_filter_list, sizeof(char**)*(superblock_filter_list_length+1));
                        if (superblock_filter_list) {
                                superblock_filter_list[superblock_filter_list_length-1] = optarg;
                                superblock_filter_list[superblock_filter_list_length] = NULL;
                        } else {
                                (void) log_oom();
                                return EXIT_FAILURE;
                        }
                        break;
                case 'p':
                        partition_filter_list_length++;
                        partition_filter_list = (char**) realloc(partition_filter_list, sizeof(char**)*(partition_filter_list_length+1));
                        if (partition_filter_list) {
                                partition_filter_list[partition_filter_list_length-1] = optarg;
                                partition_filter_list[partition_filter_list_length] = NULL;
                        } else {
                                (void) log_oom();
                                return EXIT_FAILURE;
                        }
                        break;
                }
        }

        pr = blkid_new_probe();
        if (!pr)
                return EXIT_FAILURE;

        blkid_probe_set_superblocks_flags(pr,
                BLKID_SUBLKS_LABEL | BLKID_SUBLKS_UUID |
                BLKID_SUBLKS_TYPE | BLKID_SUBLKS_SECTYPE |
                BLKID_SUBLKS_USAGE | BLKID_SUBLKS_VERSION |
                BLKID_SUBLKS_BADCSUM);

        if (superblock_filter_list)
                if (blkid_probe_filter_superblocks_type(pr, BLKID_FLTR_NOTIN, superblock_filter_list) < 0)
                        log_warning("Failed to filter superblock types.");

        if (partition_filter_list)
                if (blkid_probe_filter_partitions_type(pr, BLKID_FLTR_NOTIN, partition_filter_list) < 0)
                        log_warning("Failed to filter partition types.");

        if (noraid)
                blkid_probe_filter_superblocks_usage(pr, BLKID_FLTR_NOTIN, BLKID_USAGE_RAID);

        fd = open(udev_device_get_devnode(dev), O_RDONLY|O_CLOEXEC);
        if (fd < 0) {
                err = log_debug_errno(errno, "Failure opening block device %s: %m", udev_device_get_devnode(dev));
                goto out;
        }

        err = blkid_probe_set_device(pr, fd, offset, 0);
        if (err < 0)
                goto out;

        log_debug("probe %s %sraid offset=%"PRIi64,
                  udev_device_get_devnode(dev),
                  noraid ? "no" : "", offset);

        err = probe_superblocks(pr);
        if (err < 0)
                goto out;
        if (blkid_probe_has_value(pr, "SBBADCSUM")) {
                if (!blkid_probe_lookup_value(pr, "TYPE", &prtype, NULL))
                        log_warning("incorrect %s checksum on %s",
                                    prtype, udev_device_get_devnode(dev));
                else
                        log_warning("incorrect checksum on %s",
                                    udev_device_get_devnode(dev));
                goto out;
        }

        /* If we are a partition then our parent passed on the root
         * partition UUID to us */
        root_partition = udev_device_get_property_value(dev, "ID_PART_GPT_AUTO_ROOT_UUID");

        nvals = blkid_probe_numof_values(pr);
        for (i = 0; i < nvals; i++) {
                if (blkid_probe_get_value(pr, i, &name, &data, NULL))
                        continue;

                print_property(dev, test, name, data);

                /* Is this a disk with GPT partition table? */
                if (streq(name, "PTTYPE") && streq(data, "gpt"))
                        is_gpt = true;

                /* Is this a partition that matches the root partition
                 * property we inherited from our parent? */
                if (root_partition && streq(name, "PART_ENTRY_UUID") && streq(data, root_partition))
                        udev_builtin_add_property(dev, test, "ID_PART_GPT_AUTO_ROOT", "1");
        }

        if (is_gpt)
                find_gpt_root(dev, pr, test);

        blkid_free_probe(pr);
out:
        if (err < 0)
                return EXIT_FAILURE;

        return EXIT_SUCCESS;
}

const struct udev_builtin udev_builtin_blkid = {
        .name = "blkid",
        .cmd = builtin_blkid,
        .help = "Filesystem and partition probing",
        .run_once = true,
};
