/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>

#include "sd-device.h"
#include "sd-id128.h"

#include "alloc-util.h"
#include "blkid-util.h"
#include "blockdev-util.h"
#include "btrfs-util.h"
#include "device-util.h"
#include "devnum-util.h"
#include "dirent-util.h"
#include "dissect-image.h"
#include "dropin.h"
#include "efi-loader.h"
#include "fd-util.h"
#include "fileio.h"
#include "fs-util.h"
#include "fstab-util.h"
#include "generator.h"
#include "gpt.h"
#include "image-policy.h"
#include "initrd-util.h"
#include "mkdir.h"
#include "mountpoint-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "specifier.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "unit-name.h"
#include "virt.h"

static const char *arg_dest = NULL;
static bool arg_enabled = true;
static bool arg_root_enabled = true;
static char *arg_root_fstype = NULL;
static char *arg_root_options = NULL;
static int arg_root_rw = -1;
static ImagePolicy *arg_image_policy = NULL;

STATIC_DESTRUCTOR_REGISTER(arg_image_policy, image_policy_freep);

STATIC_DESTRUCTOR_REGISTER(arg_root_fstype, freep);
STATIC_DESTRUCTOR_REGISTER(arg_root_options, freep);

static int add_cryptsetup(
                const char *id,
                const char *what,
                bool rw,
                bool require,
                bool measure,
                char **ret_device) {

#if HAVE_LIBCRYPTSETUP
        _cleanup_free_ char *e = NULL, *n = NULL, *d = NULL, *options = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(id);
        assert(what);

        r = unit_name_from_path(what, ".device", &d);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        e = unit_name_escape(id);
        if (!e)
                return log_oom();

        r = unit_name_build("systemd-cryptsetup", e, ".service", &n);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        r = generator_open_unit_file(arg_dest, NULL, n, &f);
        if (r < 0)
                return r;

        r = generator_write_cryptsetup_unit_section(f, NULL);
        if (r < 0)
                return r;

        fprintf(f,
                "Before=umount.target cryptsetup.target\n"
                "Conflicts=umount.target\n"
                "BindsTo=%s\n"
                "After=%s\n",
                d, d);

        if (!rw) {
                options = strdup("read-only");
                if (!options)
                        return log_oom();
        }

        if (measure) {
                /* We only measure the root volume key into PCR 15 if we are booted with sd-stub (i.e. in a
                 * UKI), and sd-stub measured the UKI. We do this in order not to step into people's own PCR
                 * assignment, under the assumption that people who are fine to use sd-stub with its PCR
                 * assignments are also OK with our PCR 15 use here. */

                r = efi_stub_measured(LOG_WARNING);
                if (r == 0)
                        log_debug("Will not measure volume key of volume '%s', because not booted via systemd-stub with measurements enabled.", id);
                else if (r > 0) {
                        if (!strextend_with_separator(&options, ",", "tpm2-measure-pcr=yes"))
                                return log_oom();
                }
        }

        r = generator_write_cryptsetup_service_section(f, id, what, NULL, options);
        if (r < 0)
                return r;

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write file %s: %m", n);

        r = generator_add_symlink(arg_dest, d, "wants", n);
        if (r < 0)
                return r;

        const char *dmname;
        dmname = strjoina("dev-mapper-", e, ".device");

        if (require) {
                r = generator_add_symlink(arg_dest, "cryptsetup.target", "requires", n);
                if (r < 0)
                        return r;

                r = generator_add_symlink(arg_dest, dmname, "requires", n);
                if (r < 0)
                        return r;
        }

        r = write_drop_in_format(arg_dest, dmname, 50, "job-timeout",
                                 "# Automatically generated by systemd-gpt-auto-generator\n\n"
                                 "[Unit]\n"
                                 "JobTimeoutSec=0"); /* the binary handles timeouts anyway */
        if (r < 0)
                log_warning_errno(r, "Failed to write device timeout drop-in, ignoring: %m");

        if (ret_device) {
                char *s;

                s = path_join("/dev/mapper", id);
                if (!s)
                        return log_oom();

                *ret_device = s;
        }

        return 0;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Partition is encrypted, but the project was compiled without libcryptsetup support");
#endif
}

static int add_mount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
                bool growfs,
                bool measure,
                const char *options,
                const char *description,
                const char *post) {

        _cleanup_free_ char *unit = NULL, *crypto_what = NULL, *p = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        /* Note that we don't apply specifier escaping on the input strings here, since we know they are not configured
         * externally, but all originate from our own sources here, and hence we know they contain no % characters that
         * could potentially be understood as specifiers. */

        assert(id);
        assert(what);
        assert(where);
        assert(description);

        log_debug("Adding %s: %s fstype=%s", where, what, fstype ?: "(any)");

        if (streq_ptr(fstype, "crypto_LUKS")) {
                r = add_cryptsetup(id, what, rw, /* require= */ true, measure, &crypto_what);
                if (r < 0)
                        return r;

                what = crypto_what;
                fstype = NULL;
        } else if (fstype) {
                r = dissect_fstype_ok(fstype);
                if (r < 0)
                        return log_error_errno(r, "Unable to determine of dissected file system type '%s' is permitted: %m", fstype);
                if (!r)
                        return log_error_errno(
                                        SYNTHETIC_ERRNO(EIDRM),
                                        "Refusing to automatically mount uncommon file system '%s' to '%s'.",
                                        fstype, where);
        }

        r = unit_name_from_path(where, ".mount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = path_join(empty_to_root(arg_dest), unit);
        if (!p)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=%s\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n",
                description);

        if (post)
                fprintf(f, "Before=%s\n", post);

        r = generator_write_fsck_deps(f, arg_dest, what, where, fstype);
        if (r < 0)
                return r;

        r = generator_write_blockdev_dependency(f, what);
        if (r < 0)
                return r;

        fprintf(f,
                "\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=%s\n",
                what, where);

        if (fstype)
                fprintf(f, "Type=%s\n", fstype);

        if (options)
                fprintf(f, "Options=%s\n", options);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        if (growfs) {
                r = generator_hook_up_growfs(arg_dest, where, post);
                if (r < 0)
                        return r;
        }

        if (measure) {
                r = generator_hook_up_pcrfs(arg_dest, where, post);
                if (r < 0)
                        return r;
        }

        if (post) {
                r = generator_add_symlink(arg_dest, post, "requires", unit);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int path_is_busy(const char *where) {
        int r;

        /* already a mountpoint; generators run during reload */
        r = path_is_mount_point(where, NULL, AT_SYMLINK_FOLLOW);
        if (r > 0)
                return false;

        /* the directory might not exist on a stateless system */
        if (r == -ENOENT)
                return false;

        if (r < 0)
                return log_warning_errno(r, "Cannot check if \"%s\" is a mount point: %m", where);

        /* not a mountpoint but it contains files */
        r = dir_is_empty(where, /* ignore_hidden_or_backup= */ false);
        if (r < 0)
                return log_warning_errno(r, "Cannot check if \"%s\" is empty: %m", where);
        if (r > 0)
                return false;

        log_debug("\"%s\" already populated, ignoring.", where);
        return true;
}

static int add_partition_mount(
                PartitionDesignator d,
                DissectedPartition *p,
                const char *id,
                const char *where,
                const char *description) {

        _cleanup_free_ char *options = NULL;
        int r;

        assert(p);

        r = path_is_busy(where);
        if (r != 0)
                return r < 0 ? r : 0;

        r = partition_pick_mount_options(
                        d,
                        dissected_partition_fstype(p),
                        p->rw,
                        /* discard= */ true,
                        &options,
                        /* ret_ms_flags= */ NULL);
        if (r < 0)
                return r;

        return add_mount(
                        id,
                        p->node,
                        where,
                        p->fstype,
                        p->rw,
                        p->growfs,
                        /* measure= */ STR_IN_SET(id, "root", "var"), /* by default measure rootfs and /var, since they contain the "identity" of the system */
                        options,
                        description,
                        SPECIAL_LOCAL_FS_TARGET);
}

static int add_partition_swap(DissectedPartition *p) {
        const char *what;
        _cleanup_free_ char *name = NULL, *unit = NULL, *crypto_what = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(p);
        assert(p->node);

        /* Disable the swap auto logic if at least one swap is defined in /etc/fstab, see #6192. */
        r = fstab_has_fstype("swap");
        if (r < 0)
                return log_error_errno(r, "Failed to parse fstab: %m");
        if (r > 0) {
                log_debug("swap specified in fstab, ignoring.");
                return 0;
        }

        if (streq_ptr(p->fstype, "crypto_LUKS")) {
                r = add_cryptsetup("swap", p->node, /* rw= */ true, /* require= */ true, /* measure= */ false, &crypto_what);
                if (r < 0)
                        return r;
                what = crypto_what;
        } else
                what = p->node;

        log_debug("Adding swap: %s", what);

        r = unit_name_from_path(what, ".swap", &name);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        unit = path_join(empty_to_root(arg_dest), name);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=Swap Partition\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n");

        r = generator_write_blockdev_dependency(f, what);
        if (r < 0)
                return r;

        fprintf(f,
                "\n"
                "[Swap]\n"
                "What=%s\n",
                what);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        return generator_add_symlink(arg_dest, SPECIAL_SWAP_TARGET, "wants", name);
}

static int add_automount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
                bool growfs,
                const char *options,
                const char *description,
                usec_t timeout) {

        _cleanup_free_ char *unit = NULL, *p = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(id);
        assert(where);
        assert(description);

        r = add_mount(id,
                      what,
                      where,
                      fstype,
                      rw,
                      growfs,
                      /* measure= */ false,
                      options,
                      description,
                      NULL);
        if (r < 0)
                return r;

        r = unit_name_from_path(where, ".automount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = path_join(arg_dest, unit);
        if (!p)
                return log_oom();

        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=%s\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n"
                "[Automount]\n"
                "Where=%s\n"
                "TimeoutIdleSec="USEC_FMT"\n",
                description,
                where,
                timeout / USEC_PER_SEC);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        return generator_add_symlink(arg_dest, SPECIAL_LOCAL_FS_TARGET, "wants", unit);
}

static int add_partition_xbootldr(DissectedPartition *p) {
        _cleanup_free_ char *options = NULL;
        int r;

        assert(p);

        if (in_initrd()) {
                log_debug("In initrd, ignoring the XBOOTLDR partition.");
                return 0;
        }

        r = fstab_is_mount_point("/boot");
        if (r < 0)
                return log_error_errno(r, "Failed to parse fstab: %m");
        if (r > 0) {
                log_debug("/boot specified in fstab, ignoring XBOOTLDR partition.");
                return 0;
        }

        r = path_is_busy("/boot");
        if (r < 0)
                return r;
        if (r > 0)
                return 0;

        r = partition_pick_mount_options(
                        PARTITION_XBOOTLDR,
                        dissected_partition_fstype(p),
                        /* rw= */ true,
                        /* discard= */ false,
                        &options,
                        /* ret_ms_flags= */ NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to determine default mount options for Boot Loader Partition: %m");

        return add_automount("boot",
                             p->node,
                             "/boot",
                             p->fstype,
                             /* rw= */ true,
                             /* growfs= */ false,
                             options,
                             "Boot Loader Partition",
                             120 * USEC_PER_SEC);
}

#if ENABLE_EFI
static int add_partition_esp(DissectedPartition *p, bool has_xbootldr) {
        const char *esp_path = NULL, *id = NULL;
        _cleanup_free_ char *options = NULL;
        int r;

        assert(p);

        if (in_initrd()) {
                log_debug("In initrd, ignoring the ESP.");
                return 0;
        }

        /* If /efi exists we'll use that. Otherwise we'll use /boot, as that's usually the better choice, but
         * only if there's no explicit XBOOTLDR partition around. */
        if (access("/efi", F_OK) < 0) {
                if (errno != ENOENT)
                        return log_error_errno(errno, "Failed to determine whether /efi exists: %m");

                /* Use /boot as fallback, but only if there's no XBOOTLDR partition and /boot exists */
                if (!has_xbootldr) {
                        if (access("/boot", F_OK) < 0) {
                                if (errno != ENOENT)
                                        return log_error_errno(errno, "Failed to determine whether /boot exists: %m");
                        } else {
                                esp_path = "/boot";
                                id = "boot";
                        }
                }
        }
        if (!esp_path)
                esp_path = "/efi";
        if (!id)
                id = "efi";

        /* We create an .automount which is not overridden by the .mount from the fstab generator. */
        r = fstab_is_mount_point(esp_path);
        if (r < 0)
                return log_error_errno(r, "Failed to parse fstab: %m");
        if (r > 0) {
                log_debug("%s specified in fstab, ignoring.", esp_path);
                return 0;
        }

        r = path_is_busy(esp_path);
        if (r < 0)
                return r;
        if (r > 0)
                return 0;

        r = partition_pick_mount_options(
                        PARTITION_ESP,
                        dissected_partition_fstype(p),
                        /* rw= */ true,
                        /* discard= */ false,
                        &options,
                        /* ret_ms_flags= */ NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to determine default mount options for EFI System Partition: %m");

        return add_automount(id,
                             p->node,
                             esp_path,
                             p->fstype,
                             /* rw= */ true,
                             /* growfs= */ false,
                             options,
                             "EFI System Partition Automount",
                             120 * USEC_PER_SEC);
}
#else
static int add_partition_esp(DissectedPartition *p, bool has_xbootldr) {
        return 0;
}
#endif

static int add_partition_root_rw(DissectedPartition *p) {
        const char *path;
        int r;

        assert(p);

        if (in_initrd()) {
                log_debug("In initrd, not generating drop-in for systemd-remount-fs.service.");
                return 0;
        }

        if (arg_root_rw >= 0) {
                log_debug("Parameter ro/rw specified on kernel command line, not generating drop-in for systemd-remount-fs.service.");
                return 0;
        }

        if (!p->rw) {
                log_debug("Root partition marked read-only in GPT partition table, not generating drop-in for systemd-remount-fs.service.");
                return 0;
        }

        (void) generator_enable_remount_fs_service(arg_dest);

        path = strjoina(arg_dest, "/systemd-remount-fs.service.d/50-remount-rw.conf");

        r = write_string_file(path,
                              "# Automatically generated by systemd-gpt-auto-generator\n\n"
                              "[Service]\n"
                              "Environment=SYSTEMD_REMOUNT_ROOT_RW=1\n",
                              WRITE_STRING_FILE_CREATE|WRITE_STRING_FILE_NOFOLLOW|WRITE_STRING_FILE_MKDIR_0755);
        if (r < 0)
                return log_error_errno(r, "Failed to write drop-in file %s: %m", path);

        return 0;
}

#if ENABLE_EFI
static int add_root_cryptsetup(void) {
#if HAVE_LIBCRYPTSETUP

        /* If a device /dev/gpt-auto-root-luks appears, then make it pull in systemd-cryptsetup-root.service, which
         * sets it up, and causes /dev/gpt-auto-root to appear which is all we are looking for. */

        return add_cryptsetup("root", "/dev/gpt-auto-root-luks", /* rw= */ true, /* require= */ false, /* measure= */ true, NULL);
#else
        return 0;
#endif
}
#endif

static int add_root_mount(void) {
#if ENABLE_EFI
        _cleanup_free_ char *options = NULL;
        int r;

        if (!is_efi_boot()) {
                log_debug("Not an EFI boot, not creating root mount.");
                return 0;
        }

        r = efi_loader_get_device_part_uuid(NULL);
        if (r == -ENOENT) {
                log_notice("EFI loader partition unknown, exiting.\n"
                           "(The boot loader did not set EFI variable LoaderDevicePartUUID.)");
                return 0;
        } else if (r < 0)
                return log_error_errno(r, "Failed to read loader partition UUID: %m");

        /* OK, we have an ESP/XBOOTLDR partition, this is fantastic, so let's wait for a root device to show up.
         * A udev rule will create the link for us under the right name. */

        if (in_initrd()) {
                r = generator_write_initrd_root_device_deps(arg_dest, "/dev/gpt-auto-root");
                if (r < 0)
                        return 0;

                r = add_root_cryptsetup();
                if (r < 0)
                        return r;
        }

        /* Note that we do not need to enable systemd-remount-fs.service here. If
         * /etc/fstab exists, systemd-fstab-generator will pull it in for us. */

        r = partition_pick_mount_options(
                        PARTITION_ROOT,
                        arg_root_fstype,
                        arg_root_rw > 0,
                        /* discard= */ true,
                        &options,
                        /* ret_ms_flags= */ NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to pick root mount options: %m");

        if (arg_root_options)
                if (!strextend_with_separator(&options, ",", arg_root_options))
                        return log_oom();

        return add_mount(
                        "root",
                        "/dev/gpt-auto-root",
                        in_initrd() ? "/sysroot" : "/",
                        arg_root_fstype,
                        /* rw= */ arg_root_rw > 0,
                        /* growfs= */ false,
                        /* measure= */ true,
                        options,
                        "Root Partition",
                        in_initrd() ? SPECIAL_INITRD_ROOT_FS_TARGET : SPECIAL_LOCAL_FS_TARGET);
#else
        return 0;
#endif
}

static bool esp_is_valid(DissectedPartition *p, sd_id128_t xbootldr_uuid) {
        sd_id128_t loader_uuid;
        int r;

        if (!is_efi_boot()) {
                log_debug("Not an EFI boot, skipping ESP check.");
                return true;
        }

        r = efi_loader_get_device_part_uuid(&loader_uuid);
        if (r < 0) {
                /* Won't be able to check if this is the boot ESP, thus not mounting it */
                if (r == -ENOENT)
                        log_debug("EFI loader partition unknown, not mounting ESP.");
                else
                        log_debug_errno(r, "Failed to read loader partition UUID, ignoring: %m");

                return false;
        }

        if (sd_id128_equal(p->uuid, loader_uuid))
                return true;

        if (sd_id128_equal(xbootldr_uuid, loader_uuid)) {
                log_debug("LoaderDevicePartUUID points to XBOOTLDR partition, proceeding anyway.");
                return true;
        }

        log_debug("ESP '%s' does not appear to be the one we are booted from, ignoring.", p->node);
        return false;
}

static int enumerate_partitions(dev_t devnum) {
        _cleanup_(dissected_image_unrefp) DissectedImage *m = NULL;
        _cleanup_(loop_device_unrefp) LoopDevice *loop = NULL;
        _cleanup_free_ char *devname = NULL;
        int r, k;

        r = block_get_whole_disk(devnum, &devnum);
        if (r < 0)
                return log_debug_errno(r, "Failed to get whole block device for " DEVNUM_FORMAT_STR ": %m",
                                       DEVNUM_FORMAT_VAL(devnum));

        r = devname_from_devnum(S_IFBLK, devnum, &devname);
        if (r < 0)
                return log_debug_errno(r, "Failed to get device node of " DEVNUM_FORMAT_STR ": %m",
                                       DEVNUM_FORMAT_VAL(devnum));

        /* Let's take a LOCK_SH lock on the block device, in case udevd is already running. If we don't take
         * the lock, udevd might end up issuing BLKRRPART in the middle, and we don't want that, since that
         * might remove all partitions while we are operating on them. */
        r = loop_device_open_from_path(devname, O_RDONLY, LOCK_SH, &loop);
        if (r < 0)
                return log_debug_errno(r, "Failed to open %s: %m", devname);

        r = dissect_loop_device(
                        loop,
                        /* verity= */ NULL,
                        /* mount_options= */ NULL,
                        arg_image_policy ?: &image_policy_host,
                        DISSECT_IMAGE_GPT_ONLY|
                        DISSECT_IMAGE_USR_NO_ROOT|
                        DISSECT_IMAGE_DISKSEQ_DEVNODE|
                        DISSECT_IMAGE_ALLOW_EMPTY,
                        /* NB! Unlike most other places where we dissect block devices we do not use
                         * DISSECT_IMAGE_ADD_PARTITION_DEVICES here: we want that the kernel finds the
                         * devices, and udev probes them before we mount them via .mount units much later
                         * on. And thus we also don't set DISSECT_IMAGE_PIN_PARTITION_DEVICES here, because
                         * we don't actually mount anything immediately. */
                        &m);
        if (r < 0) {
                bool ok = r == -ENOPKG;
                dissect_log_error(ok ? LOG_DEBUG : LOG_ERR, r, devname, NULL);
                return ok ? 0 : r;
        }

        if (m->partitions[PARTITION_SWAP].found) {
                k = add_partition_swap(m->partitions + PARTITION_SWAP);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_XBOOTLDR].found) {
                k = add_partition_xbootldr(m->partitions + PARTITION_XBOOTLDR);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_ESP].found &&
            esp_is_valid(m->partitions + PARTITION_ESP,
                         m->partitions[PARTITION_XBOOTLDR].uuid)) {

                k = add_partition_esp(m->partitions + PARTITION_ESP, m->partitions[PARTITION_XBOOTLDR].found);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_HOME].found) {
                k = add_partition_mount(PARTITION_HOME, m->partitions + PARTITION_HOME, "home", "/home", "Home Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_SRV].found) {
                k = add_partition_mount(PARTITION_SRV, m->partitions + PARTITION_SRV, "srv", "/srv", "Server Data Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_VAR].found) {
                k = add_partition_mount(PARTITION_VAR, m->partitions + PARTITION_VAR, "var", "/var", "Variable Data Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_TMP].found) {
                k = add_partition_mount(PARTITION_TMP, m->partitions + PARTITION_TMP, "var-tmp", "/var/tmp", "Temporary Data Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_ROOT].found) {
                k = add_partition_root_rw(m->partitions + PARTITION_ROOT);
                if (k < 0)
                        r = k;
        }

        return r;
}

static int add_mounts(void) {
        dev_t devno;
        int r;

        r = blockdev_get_root(LOG_ERR, &devno);
        if (r < 0)
                return r;
        if (r == 0) {
                log_debug("Skipping automatic GPT dissection logic, root file system not backed by a (single) whole block device.");
                return 0;
        }

        return enumerate_partitions(devno);
}

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        assert(key);

        if (proc_cmdline_key_streq(key, "systemd.gpt_auto") ||
            proc_cmdline_key_streq(key, "rd.systemd.gpt_auto")) {

                r = value ? parse_boolean(value) : 1;
                if (r < 0)
                        log_warning_errno(r, "Failed to parse gpt-auto switch \"%s\", ignoring: %m", value);
                else
                        arg_enabled = r;

        } else if (proc_cmdline_key_streq(key, "root")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                /* Disable root disk logic if there's a root= value
                 * specified (unless it happens to be "gpt-auto") */

                if (!streq(value, "gpt-auto")) {
                        arg_root_enabled = false;
                        log_debug("Disabling root partition auto-detection, root= is defined.");
                }

        } else if (proc_cmdline_key_streq(key, "roothash")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                /* Disable root disk logic if there's roothash= defined (i.e. verity enabled) */

                arg_root_enabled = false;

        } else if (streq(key, "rootfstype")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                return free_and_strdup_warn(&arg_root_fstype, value);

        } else if (streq(key, "rootflags")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                if (!strextend_with_separator(&arg_root_options, ",", value))
                        return log_oom();

        } else if (proc_cmdline_key_streq(key, "rw") && !value)
                arg_root_rw = true;
        else if (proc_cmdline_key_streq(key, "ro") && !value)
                arg_root_rw = false;
        else if (proc_cmdline_key_streq(key, "systemd.image_policy"))
                return parse_image_policy_argument(optarg, &arg_image_policy);

        return 0;
}

static int run(const char *dest, const char *dest_early, const char *dest_late) {
        int r, k;

        assert_se(arg_dest = dest_late);

        if (detect_container() > 0) {
                log_debug("In a container, exiting.");
                return 0;
        }

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, 0);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (!arg_enabled) {
                log_debug("Disabled, exiting.");
                return 0;
        }

        if (arg_root_enabled)
                r = add_root_mount();

        if (!in_initrd()) {
                k = add_mounts();
                if (r >= 0)
                        r = k;
        }

        return r;
}

DEFINE_MAIN_GENERATOR_FUNCTION(run);
