/* SPDX-License-Identifier: LGPL-2.1+ */

#include <blkid.h>
#include <stdlib.h>
#include <sys/statfs.h>
#include <unistd.h>

#include "sd-device.h"
#include "sd-id128.h"

#include "alloc-util.h"
#include "blkid-util.h"
#include "blockdev-util.h"
#include "btrfs-util.h"
#include "dirent-util.h"
#include "dissect-image.h"
#include "efivars.h"
#include "fd-util.h"
#include "fileio.h"
#include "fstab-util.h"
#include "generator.h"
#include "gpt.h"
#include "missing.h"
#include "mkdir.h"
#include "mount-util.h"
#include "parse-util.h"
#include "path-util.h"
#include "proc-cmdline.h"
#include "special.h"
#include "specifier.h"
#include "stat-util.h"
#include "string-util.h"
#include "strv.h"
#include "unit-name.h"
#include "util.h"
#include "virt.h"

static const char *arg_dest = "/tmp";
static bool arg_enabled = true;
static bool arg_root_enabled = true;
static bool arg_root_rw = false;

static int add_cryptsetup(const char *id, const char *what, bool rw, bool require, char **device) {
        _cleanup_free_ char *e = NULL, *n = NULL, *d = NULL, *id_escaped = NULL, *what_escaped = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *p;
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

        id_escaped = specifier_escape(id);
        if (!id_escaped)
                return log_oom();

        what_escaped = specifier_escape(what);
        if (!what_escaped)
                return log_oom();

        p = strjoina(arg_dest, "/", n);
        f = fopen(p, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", p);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=Cryptography Setup for %%I\n"
                "Documentation=man:systemd-gpt-auto-generator(8) man:systemd-cryptsetup@.service(8)\n"
                "DefaultDependencies=no\n"
                "Conflicts=umount.target\n"
                "BindsTo=dev-mapper-%%i.device %s\n"
                "Before=umount.target cryptsetup.target\n"
                "After=%s\n"
                "IgnoreOnIsolate=true\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "TimeoutSec=0\n" /* the binary handles timeouts anyway */
                "KeyringMode=shared\n" /* make sure we can share cached keys among instances */
                "ExecStart=" SYSTEMD_CRYPTSETUP_PATH " attach '%s' '%s' '' '%s'\n"
                "ExecStop=" SYSTEMD_CRYPTSETUP_PATH " detach '%s'\n",
                d, d,
                id_escaped, what_escaped, rw ? "" : "read-only",
                id_escaped);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write file %s: %m", p);

        r = generator_add_symlink(arg_dest, d, "wants", n);
        if (r < 0)
                return r;

        if (require) {
                const char *dmname;

                r = generator_add_symlink(arg_dest, "cryptsetup.target", "requires", n);
                if (r < 0)
                        return r;

                dmname = strjoina("dev-mapper-", e, ".device");
                r = generator_add_symlink(arg_dest, dmname, "requires", n);
                if (r < 0)
                        return r;
        }

        p = strjoina(arg_dest, "/dev-mapper-", e, ".device.d/50-job-timeout-sec-0.conf");
        mkdir_parents_label(p, 0755);
        r = write_string_file(p,
                        "# Automatically generated by systemd-gpt-auto-generator\n\n"
                        "[Unit]\n"
                        "JobTimeoutSec=0\n",
                        WRITE_STRING_FILE_CREATE); /* the binary handles timeouts anyway */
        if (r < 0)
                return log_error_errno(r, "Failed to write device drop-in: %m");

        if (device) {
                char *ret;

                ret = strappend("/dev/mapper/", id);
                if (!ret)
                        return log_oom();

                *device = ret;
        }

        return 0;
}

static int add_mount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
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

        log_debug("Adding %s: %s %s", where, what, strna(fstype));

        if (streq_ptr(fstype, "crypto_LUKS")) {

                r = add_cryptsetup(id, what, rw, true, &crypto_what);
                if (r < 0)
                        return r;

                what = crypto_what;
                fstype = NULL;
        }

        r = unit_name_from_path(where, ".mount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = strjoin(arg_dest, "/", unit);
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

        fprintf(f,
                "\n"
                "[Mount]\n"
                "What=%s\n"
                "Where=%s\n",
                what, where);

        if (fstype)
                fprintf(f, "Type=%s\n", fstype);

        if (options)
                fprintf(f, "Options=%s,%s\n", options, rw ? "rw" : "ro");
        else
                fprintf(f, "Options=%s\n", rw ? "rw" : "ro");

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", p);

        if (post)
                return generator_add_symlink(arg_dest, post, "requires", unit);
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
        r = dir_is_empty(where);
        if (r < 0)
                return log_warning_errno(r, "Cannot check if \"%s\" is empty: %m", where);
        if (r > 0)
                return false;

        log_debug("\"%s\" already populated, ignoring.", where);
        return true;
}

static int add_partition_mount(
                DissectedPartition *p,
                const char *id,
                const char *where,
                const char *description) {

        int r;
        assert(p);

        r = path_is_busy(where);
        if (r != 0)
                return r < 0 ? r : 0;

        return add_mount(
                        id,
                        p->node,
                        where,
                        p->fstype,
                        p->rw,
                        NULL,
                        description,
                        SPECIAL_LOCAL_FS_TARGET);
}

static int add_swap(const char *path) {
        _cleanup_free_ char *name = NULL, *unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        assert(path);

        /* Disable the swap auto logic if at least one swap is defined in /etc/fstab, see #6192. */
        r = fstab_has_fstype("swap");
        if (r < 0)
                return log_error_errno(r, "Failed to parse fstab: %m");
        if (r > 0) {
                log_debug("swap specified in fstab, ignoring.");
                return 0;
        }

        log_debug("Adding swap: %s", path);

        r = unit_name_from_path(path, ".swap", &name);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        unit = strjoin(arg_dest, "/", name);
        if (!unit)
                return log_oom();

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by systemd-gpt-auto-generator\n\n"
                "[Unit]\n"
                "Description=Swap Partition\n"
                "Documentation=man:systemd-gpt-auto-generator(8)\n\n"
                "[Swap]\n"
                "What=%s\n",
                path);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        return generator_add_symlink(arg_dest, SPECIAL_SWAP_TARGET, "wants", name);
}

#if ENABLE_EFI
static int add_automount(
                const char *id,
                const char *what,
                const char *where,
                const char *fstype,
                bool rw,
                const char *options,
                const char *description,
                usec_t timeout) {

        _cleanup_free_ char *unit = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        const char *opt = "noauto", *p;
        int r;

        assert(id);
        assert(where);
        assert(description);

        if (options)
                opt = strjoina(options, ",", opt);

        r = add_mount(id,
                      what,
                      where,
                      fstype,
                      rw,
                      opt,
                      description,
                      NULL);
        if (r < 0)
                return r;

        r = unit_name_from_path(where, ".automount", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit name: %m");

        p = strjoina(arg_dest, "/", unit);
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

static int add_esp(DissectedPartition *p) {
        const char *esp;
        int r;

        assert(p);

        if (in_initrd()) {
                log_debug("In initrd, ignoring the ESP.");
                return 0;
        }

        /* If /efi exists we'll use that. Otherwise we'll use /boot, as that's usually the better choice */
        esp = access("/efi/", F_OK) >= 0 ? "/efi" : "/boot";

        /* We create an .automount which is not overridden by the .mount from the fstab generator. */
        r = fstab_is_mount_point(esp);
        if (r < 0)
                return log_error_errno(r, "Failed to parse fstab: %m");
        if (r > 0) {
                log_debug("%s specified in fstab, ignoring.", esp);
                return 0;
        }

        r = path_is_busy(esp);
        if (r != 0)
                return r < 0 ? r : 0;

        if (is_efi_boot()) {
                sd_id128_t loader_uuid;

                /* If this is an EFI boot, be extra careful, and only mount the ESP if it was the ESP used for booting. */

                r = efi_loader_get_device_part_uuid(&loader_uuid);
                if (r == -ENOENT) {
                        log_debug("EFI loader partition unknown.");
                        return 0;
                }
                if (r < 0)
                        return log_error_errno(r, "Failed to read ESP partition UUID: %m");

                if (!sd_id128_equal(p->uuid, loader_uuid)) {
                        log_debug("Partition for %s does not appear to be the partition we are booted from.", esp);
                        return 0;
                }
        } else
                log_debug("Not an EFI boot, skipping ESP check.");

        return add_automount("boot",
                             p->node,
                             esp,
                             p->fstype,
                             true,
                             "umask=0077",
                             "EFI System Partition Automount",
                             120 * USEC_PER_SEC);
}
#else
static int add_esp(DissectedPartition *p) {
        return 0;
}
#endif

static int open_parent(dev_t devnum, int *ret) {
        _cleanup_(sd_device_unrefp) sd_device *d = NULL;
        const char *name, *devtype, *node;
        sd_device *parent;
        dev_t pn;
        int fd, r;

        assert(ret);

        r = sd_device_new_from_devnum(&d, 'b', devnum);
        if (r < 0)
                return log_debug_errno(r, "Failed to open device: %m");

        if (sd_device_get_devname(d, &name) < 0) {
                r = sd_device_get_syspath(d, &name);
                if (r < 0) {
                        log_debug_errno(r, "Device %u:%u does not have a name, ignoring: %m", major(devnum), minor(devnum));
                        goto not_found;
                }
        }

        r = sd_device_get_parent(d, &parent);
        if (r < 0) {
                log_debug_errno(r, "%s: not a partitioned device, ignoring: %m", name);
                goto not_found;
        }

        /* Does it have a devtype? */
        r = sd_device_get_devtype(parent, &devtype);
        if (r < 0 || !devtype) {
                log_debug("%s: parent doesn't have a device type, ignoring", name);
                goto not_found;
        }

        /* Is this a disk or a partition? We only care for disks... */
        if (!streq(devtype, "disk")) {
                log_debug("%s: parent isn't a raw disk, ignoring.", name);
                goto not_found;
        }

        /* Does it have a device node? */
        r = sd_device_get_devname(parent, &node);
        if (r < 0) {
                log_debug_errno(r, "%s: parent device does not have device node, ignoring: %m", name);
                goto not_found;
        }

        log_debug("%s: root device %s.", name, node);

        r = sd_device_get_devnum(parent, &pn);
        if (r < 0 || major(pn) == 0) {
                log_debug("%s: parent device is not a proper block device, ignoring", name);
                goto not_found;
        }

        fd = open(node, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fd < 0)
                return log_error_errno(errno, "Failed to open %s: %m", node);

        *ret = fd;
        return 1;

not_found:
        *ret = -1;
        return 0;
}

static int enumerate_partitions(dev_t devnum) {

        _cleanup_close_ int fd = -1;
        _cleanup_(dissected_image_unrefp) DissectedImage *m = NULL;
        int r, k;

        r = open_parent(devnum, &fd);
        if (r <= 0)
                return r;

        r = dissect_image(fd, NULL, 0, DISSECT_IMAGE_GPT_ONLY, &m);
        if (r == -ENOPKG) {
                log_debug_errno(r, "No suitable partition table found, ignoring.");
                return 0;
        }
        if (r < 0)
                return log_error_errno(r, "Failed to dissect: %m");

        if (m->partitions[PARTITION_SWAP].found) {
                k = add_swap(m->partitions[PARTITION_SWAP].node);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_ESP].found) {
                k = add_esp(m->partitions + PARTITION_ESP);
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_HOME].found) {
                k = add_partition_mount(m->partitions + PARTITION_HOME, "home", "/home", "Home Partition");
                if (k < 0)
                        r = k;
        }

        if (m->partitions[PARTITION_SRV].found) {
                k = add_partition_mount(m->partitions + PARTITION_SRV, "srv", "/srv", "Server Data Partition");
                if (k < 0)
                        r = k;
        }

        return r;
}

static int parse_proc_cmdline_item(const char *key, const char *value, void *data) {
        int r;

        assert(key);

        if (STR_IN_SET(key, "systemd.gpt_auto", "rd.systemd.gpt_auto")) {

                r = value ? parse_boolean(value) : 1;
                if (r < 0)
                        log_warning("Failed to parse gpt-auto switch \"%s\". Ignoring.", value);
                else
                        arg_enabled = r;

        } else if (streq(key, "root")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                /* Disable root disk logic if there's a root= value
                 * specified (unless it happens to be "gpt-auto") */

                arg_root_enabled = streq(value, "gpt-auto");

        } else if (streq(key, "roothash")) {

                if (proc_cmdline_value_missing(key, value))
                        return 0;

                /* Disable root disk logic if there's roothash= defined (i.e. verity enabled) */

                arg_root_enabled = false;

        } else if (streq(key, "rw") && !value)
                arg_root_rw = true;
        else if (streq(key, "ro") && !value)
                arg_root_rw = false;

        return 0;
}

#if ENABLE_EFI
static int add_root_cryptsetup(void) {

        /* If a device /dev/gpt-auto-root-luks appears, then make it pull in systemd-cryptsetup-root.service, which
         * sets it up, and causes /dev/gpt-auto-root to appear which is all we are looking for. */

        return add_cryptsetup("root", "/dev/gpt-auto-root-luks", true, false, NULL);
}
#endif

static int add_root_mount(void) {

#if ENABLE_EFI
        int r;

        if (!is_efi_boot()) {
                log_debug("Not a EFI boot, not creating root mount.");
                return 0;
        }

        r = efi_loader_get_device_part_uuid(NULL);
        if (r == -ENOENT) {
                log_debug("EFI loader partition unknown, exiting.");
                return 0;
        } else if (r < 0)
                return log_error_errno(r, "Failed to read ESP partition UUID: %m");

        /* OK, we have an ESP partition, this is fantastic, so let's
         * wait for a root device to show up. A udev rule will create
         * the link for us under the right name. */

        if (in_initrd()) {
                r = generator_write_initrd_root_device_deps(arg_dest, "/dev/gpt-auto-root");
                if (r < 0)
                        return 0;

                r = add_root_cryptsetup();
                if (r < 0)
                        return r;
        }

        return add_mount(
                        "root",
                        "/dev/gpt-auto-root",
                        in_initrd() ? "/sysroot" : "/",
                        NULL,
                        arg_root_rw,
                        NULL,
                        "Root Partition",
                        in_initrd() ? SPECIAL_INITRD_ROOT_FS_TARGET : SPECIAL_LOCAL_FS_TARGET);
#else
        return 0;
#endif
}

static int add_mounts(void) {
        dev_t devno;
        int r;

        r = get_block_device_harder("/", &devno);
        if (r < 0)
                return log_error_errno(r, "Failed to determine block device of root file system: %m");
        if (r == 0) {
                r = get_block_device_harder("/usr", &devno);
                if (r < 0)
                        return log_error_errno(r, "Failed to determine block device of /usr file system: %m");
                if (r == 0) {
                        log_debug("Neither root nor /usr file system are on a (single) block device.");
                        return 0;
                }
        }

        return enumerate_partitions(devno);
}

int main(int argc, char *argv[]) {
        int r, k;

        if (argc > 1 && argc != 4) {
                log_error("This program takes three or no arguments.");
                return EXIT_FAILURE;
        }

        if (argc > 1)
                arg_dest = argv[3];

        log_set_prohibit_ipc(true);
        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        umask(0022);

        if (detect_container() > 0) {
                log_debug("In a container, exiting.");
                return EXIT_SUCCESS;
        }

        r = proc_cmdline_parse(parse_proc_cmdline_item, NULL, 0);
        if (r < 0)
                log_warning_errno(r, "Failed to parse kernel command line, ignoring: %m");

        if (!arg_enabled) {
                log_debug("Disabled, exiting.");
                return EXIT_SUCCESS;
        }

        if (arg_root_enabled)
                r = add_root_mount();
        else
                r = 0;

        if (!in_initrd()) {
                k = add_mounts();
                if (k < 0)
                        r = k;
        }

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
