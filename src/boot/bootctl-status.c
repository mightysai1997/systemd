/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/mman.h>
#include <unistd.h>

#include "bootctl.h"
#include "bootctl-status.h"
#include "bootctl-util.h"
#include "bootspec.h"
#include "chase-symlinks.h"
#include "devnum-util.h"
#include "dirent-util.h"
#include "efi-api.h"
#include "efi-loader.h"
#include "errno-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "find-esp.h"
#include "path-util.h"
#include "pretty-print.h"
#include "terminal-util.h"
#include "tpm2-util.h"
#include "recurse-dir.h"

static int boot_config_load_and_select(
                BootConfig *config,
                const char *esp_path,
                dev_t esp_devid,
                const char *xbootldr_path,
                dev_t xbootldr_devid) {

        int r;

        /* If XBOOTLDR and ESP actually refer to the same block device, suppress XBOOTLDR, since it would
         * find the same entries twice. */
        bool same = esp_path && xbootldr_path && devnum_set_and_equal(esp_devid, xbootldr_devid);

        r = boot_config_load(config, esp_path, same ? NULL : xbootldr_path);
        if (r < 0)
                return r;

        if (!arg_root) {
                _cleanup_strv_free_ char **efi_entries = NULL;

                r = efi_loader_get_entries(&efi_entries);
                if (r == -ENOENT || ERRNO_IS_NOT_SUPPORTED(r))
                        log_debug_errno(r, "Boot loader reported no entries.");
                else if (r < 0)
                        log_warning_errno(r, "Failed to determine entries reported by boot loader, ignoring: %m");
                else
                        (void) boot_config_augment_from_loader(config, efi_entries, /* only_auto= */ false);
        }

        return boot_config_select_special_entries(config, /* skip_efivars= */ !!arg_root);
}

static int status_entries(
                const BootConfig *config,
                const char *esp_path,
                sd_id128_t esp_partition_uuid,
                const char *xbootldr_path,
                sd_id128_t xbootldr_partition_uuid) {

        sd_id128_t dollar_boot_partition_uuid;
        const char *dollar_boot_path;
        int r;

        assert(config);
        assert(esp_path || xbootldr_path);

        if (xbootldr_path) {
                dollar_boot_path = xbootldr_path;
                dollar_boot_partition_uuid = xbootldr_partition_uuid;
        } else {
                dollar_boot_path = esp_path;
                dollar_boot_partition_uuid = esp_partition_uuid;
        }

        printf("%sBoot Loader Entries:%s\n"
               "        $BOOT: %s", ansi_underline(), ansi_normal(), dollar_boot_path);
        if (!sd_id128_is_null(dollar_boot_partition_uuid))
                printf(" (/dev/disk/by-partuuid/" SD_ID128_UUID_FORMAT_STR ")",
                       SD_ID128_FORMAT_VAL(dollar_boot_partition_uuid));
        if (settle_entry_token() == 0)
                printf("\n        token: %s", arg_entry_token);
        printf("\n\n");

        if (config->default_entry < 0)
                printf("%zu entries, no entry could be determined as default.\n", config->n_entries);
        else {
                printf("%sDefault Boot Loader Entry:%s\n", ansi_underline(), ansi_normal());

                r = show_boot_entry(
                                boot_config_default_entry(config),
                                /* show_as_default= */ false,
                                /* show_as_selected= */ false,
                                /* show_discovered= */ false);
                if (r > 0)
                        /* < 0 is already logged by the function itself, let's just emit an extra warning if
                           the default entry is broken */
                        printf("\nWARNING: default boot entry is broken\n");
        }

        return 0;
}

static int print_efi_option(uint16_t id, int *n_printed, bool in_order) {
        _cleanup_free_ char *title = NULL;
        _cleanup_free_ char *path = NULL;
        sd_id128_t partition;
        bool active;
        int r;

        assert(n_printed);

        r = efi_get_boot_option(id, &title, &partition, &path, &active);
        if (r == -ENOENT) {
                log_debug_errno(r, "Boot option 0x%04X referenced but missing, ignoring: %m", id);
                return 0;
        }
        if (r < 0)
                return log_error_errno(r, "Failed to read boot option 0x%04X: %m", id);

        /* print only configured entries with partition information */
        if (!path || sd_id128_is_null(partition)) {
                log_debug("Ignoring boot entry 0x%04X without partition information.", id);
                return 0;
        }

        efi_tilt_backslashes(path);

        if (*n_printed == 0) /* Print section title before first entry */
                printf("%sBoot Loaders Listed in EFI Variables:%s\n", ansi_underline(), ansi_normal());

        printf("        Title: %s%s%s\n", ansi_highlight(), strna(title), ansi_normal());
        printf("           ID: 0x%04X\n", id);
        printf("       Status: %sactive%s\n", active ? "" : "in", in_order ? ", boot-order" : "");
        printf("    Partition: /dev/disk/by-partuuid/" SD_ID128_UUID_FORMAT_STR "\n",
               SD_ID128_FORMAT_VAL(partition));
        printf("         File: %s%s\n", special_glyph(SPECIAL_GLYPH_TREE_RIGHT), path);
        printf("\n");

        (*n_printed)++;
        return 1;
}

static int status_variables(void) {
        _cleanup_free_ uint16_t *options = NULL, *order = NULL;
        int n_options, n_order, n_printed = 0;

        n_options = efi_get_boot_options(&options);
        if (n_options == -ENOENT)
                return log_error_errno(n_options,
                                       "Failed to access EFI variables, efivarfs"
                                       " needs to be available at /sys/firmware/efi/efivars/.");
        if (n_options < 0)
                return log_error_errno(n_options, "Failed to read EFI boot entries: %m");

        n_order = efi_get_boot_order(&order);
        if (n_order == -ENOENT)
                n_order = 0;
        else if (n_order < 0)
                return log_error_errno(n_order, "Failed to read EFI boot order: %m");

        /* print entries in BootOrder first */
        for (int i = 0; i < n_order; i++)
                (void) print_efi_option(order[i], &n_printed, /* in_order= */ true);

        /* print remaining entries */
        for (int i = 0; i < n_options; i++) {
                for (int j = 0; j < n_order; j++)
                        if (options[i] == order[j])
                                goto next_option;

                (void) print_efi_option(options[i], &n_printed, /* in_order= */ false);

        next_option:
                continue;
        }

        if (n_printed == 0)
                printf("No boot loaders listed in EFI Variables.\n\n");

        return 0;
}

static int enumerate_binaries(
                const char *esp_path,
                const char *path,
                const char *prefix,
                char **previous,
                bool *is_first) {

        _cleanup_closedir_ DIR *d = NULL;
        _cleanup_free_ char *p = NULL;
        int c = 0, r;

        assert(esp_path);
        assert(path);
        assert(previous);
        assert(is_first);

        r = chase_symlinks_and_opendir(path, esp_path, CHASE_PREFIX_ROOT|CHASE_PROHIBIT_SYMLINKS, &p, &d);
        if (r == -ENOENT)
                return 0;
        if (r < 0)
                return log_error_errno(r, "Failed to read \"%s/%s\": %m", esp_path, path);

        FOREACH_DIRENT(de, d, break) {
                _cleanup_free_ char *v = NULL;
                _cleanup_close_ int fd = -EBADF;

                if (!endswith_no_case(de->d_name, ".efi"))
                        continue;

                if (prefix && !startswith_no_case(de->d_name, prefix))
                        continue;

                fd = openat(dirfd(d), de->d_name, O_RDONLY|O_CLOEXEC);
                if (fd < 0)
                        return log_error_errno(errno, "Failed to open \"%s/%s\" for reading: %m", p, de->d_name);

                r = get_file_version(fd, &v);
                if (r < 0)
                        return r;

                if (*previous) { /* let's output the previous entry now, since now we know that there will be one more, and can draw the tree glyph properly */
                        printf("         %s %s%s\n",
                               *is_first ? "File:" : "     ",
                               special_glyph(SPECIAL_GLYPH_TREE_BRANCH), *previous);
                        *is_first = false;
                        *previous = mfree(*previous);
                }

                /* Do not output this entry immediately, but store what should be printed in a state
                 * variable, because we only will know the tree glyph to print (branch or final edge) once we
                 * read one more entry */
                if (r > 0)
                        r = asprintf(previous, "/%s/%s (%s%s%s)", path, de->d_name, ansi_highlight(), v, ansi_normal());
                else
                        r = asprintf(previous, "/%s/%s", path, de->d_name);
                if (r < 0)
                        return log_oom();

                c++;
        }

        return c;
}

static int status_binaries(const char *esp_path, sd_id128_t partition) {
        _cleanup_free_ char *last = NULL;
        bool is_first = true;
        int r, k;

        printf("%sAvailable Boot Loaders on ESP:%s\n", ansi_underline(), ansi_normal());

        if (!esp_path) {
                printf("          ESP: Cannot find or access mount point of ESP.\n\n");
                return -ENOENT;
        }

        printf("          ESP: %s", esp_path);
        if (!sd_id128_is_null(partition))
                printf(" (/dev/disk/by-partuuid/" SD_ID128_UUID_FORMAT_STR ")", SD_ID128_FORMAT_VAL(partition));
        printf("\n");

        r = enumerate_binaries(esp_path, "EFI/systemd", NULL, &last, &is_first);
        if (r < 0) {
                printf("\n");
                return r;
        }

        k = enumerate_binaries(esp_path, "EFI/BOOT", "boot", &last, &is_first);
        if (k < 0) {
                printf("\n");
                return k;
        }

        if (last) /* let's output the last entry now, since now we know that there will be no more, and can draw the tree glyph properly */
                printf("         %s %s%s\n",
                       is_first ? "File:" : "     ",
                       special_glyph(SPECIAL_GLYPH_TREE_RIGHT), last);

        if (r == 0 && !arg_quiet)
                log_info("systemd-boot not installed in ESP.");
        if (k == 0 && !arg_quiet)
                log_info("No default/fallback boot loader installed in ESP.");

        printf("\n");
        return 0;
}

static void read_efi_var(const char *variable, char **ret) {
        int r;

        r = efi_get_variable_string(variable, ret);
        if (r < 0 && r != -ENOENT)
                log_warning_errno(r, "Failed to read EFI variable %s: %m", variable);
}

static void print_yes_no_line(bool first, bool good, const char *name) {
        printf("%s%s %s\n",
               first ? "     Features: " : "               ",
               COLOR_MARK_BOOL(good),
               name);
}

int verb_status(int argc, char *argv[], void *userdata) {
        sd_id128_t esp_uuid = SD_ID128_NULL, xbootldr_uuid = SD_ID128_NULL;
        dev_t esp_devid = 0, xbootldr_devid = 0;
        int r, k;

        r = acquire_esp(/* unprivileged_mode= */ geteuid() != 0, /* graceful= */ false, NULL, NULL, NULL, &esp_uuid, &esp_devid);
        if (arg_print_esp_path) {
                if (r == -EACCES) /* If we couldn't acquire the ESP path, log about access errors (which is the only
                                   * error the find_esp_and_warn() won't log on its own) */
                        return log_error_errno(r, "Failed to determine ESP location: %m");
                if (r < 0)
                        return r;

                puts(arg_esp_path);
        }

        r = acquire_xbootldr(/* unprivileged_mode= */ geteuid() != 0, &xbootldr_uuid, &xbootldr_devid);
        if (arg_print_dollar_boot_path) {
                if (r == -EACCES)
                        return log_error_errno(r, "Failed to determine XBOOTLDR partition: %m");
                if (r < 0)
                        return r;

                const char *path = arg_dollar_boot_path();
                if (!path)
                        return log_error_errno(SYNTHETIC_ERRNO(EACCES), "Failed to determine XBOOTLDR location: %m");

                puts(path);
        }

        if (arg_print_esp_path || arg_print_dollar_boot_path)
                return 0;

        r = 0; /* If we couldn't determine the path, then don't consider that a problem from here on, just
                * show what we can show */

        pager_open(arg_pager_flags);

        if (!arg_root && is_efi_boot()) {
                static const struct {
                        uint64_t flag;
                        const char *name;
                } loader_flags[] = {
                        { EFI_LOADER_FEATURE_BOOT_COUNTING,           "Boot counting"                         },
                        { EFI_LOADER_FEATURE_CONFIG_TIMEOUT,          "Menu timeout control"                  },
                        { EFI_LOADER_FEATURE_CONFIG_TIMEOUT_ONE_SHOT, "One-shot menu timeout control"         },
                        { EFI_LOADER_FEATURE_ENTRY_DEFAULT,           "Default entry control"                 },
                        { EFI_LOADER_FEATURE_ENTRY_ONESHOT,           "One-shot entry control"                },
                        { EFI_LOADER_FEATURE_XBOOTLDR,                "Support for XBOOTLDR partition"        },
                        { EFI_LOADER_FEATURE_RANDOM_SEED,             "Support for passing random seed to OS" },
                        { EFI_LOADER_FEATURE_LOAD_DRIVER,             "Load drop-in drivers"                  },
                        { EFI_LOADER_FEATURE_SORT_KEY,                "Support Type #1 sort-key field"        },
                        { EFI_LOADER_FEATURE_SAVED_ENTRY,             "Support @saved pseudo-entry"           },
                        { EFI_LOADER_FEATURE_DEVICETREE,              "Support Type #1 devicetree field"      },
                };
                static const struct {
                        uint64_t flag;
                        const char *name;
                } stub_flags[] = {
                        { EFI_STUB_FEATURE_REPORT_BOOT_PARTITION,     "Stub sets ESP information"                            },
                        { EFI_STUB_FEATURE_PICK_UP_CREDENTIALS,       "Picks up credentials from boot partition"             },
                        { EFI_STUB_FEATURE_PICK_UP_SYSEXTS,           "Picks up system extension images from boot partition" },
                        { EFI_STUB_FEATURE_THREE_PCRS,                "Measures kernel+command line+sysexts"                 },
                        { EFI_STUB_FEATURE_RANDOM_SEED,               "Support for passing random seed to OS"                },
                };
                _cleanup_free_ char *fw_type = NULL, *fw_info = NULL, *loader = NULL, *loader_path = NULL, *stub = NULL;
                sd_id128_t loader_part_uuid = SD_ID128_NULL;
                uint64_t loader_features = 0, stub_features = 0;
                Tpm2Support s;
                int have;

                read_efi_var(EFI_LOADER_VARIABLE(LoaderFirmwareType), &fw_type);
                read_efi_var(EFI_LOADER_VARIABLE(LoaderFirmwareInfo), &fw_info);
                read_efi_var(EFI_LOADER_VARIABLE(LoaderInfo), &loader);
                read_efi_var(EFI_LOADER_VARIABLE(StubInfo), &stub);
                read_efi_var(EFI_LOADER_VARIABLE(LoaderImageIdentifier), &loader_path);
                (void) efi_loader_get_features(&loader_features);
                (void) efi_stub_get_features(&stub_features);

                if (loader_path)
                        efi_tilt_backslashes(loader_path);

                k = efi_loader_get_device_part_uuid(&loader_part_uuid);
                if (k < 0 && k != -ENOENT)
                        r = log_warning_errno(k, "Failed to read EFI variable LoaderDevicePartUUID: %m");

                SecureBootMode secure = efi_get_secure_boot_mode();
                printf("%sSystem:%s\n", ansi_underline(), ansi_normal());
                printf("      Firmware: %s%s (%s)%s\n", ansi_highlight(), strna(fw_type), strna(fw_info), ansi_normal());
                printf(" Firmware Arch: %s\n", get_efi_arch());
                printf("   Secure Boot: %sd (%s)\n",
                       enable_disable(IN_SET(secure, SECURE_BOOT_USER, SECURE_BOOT_DEPLOYED)),
                       secure_boot_mode_to_string(secure));

                s = tpm2_support();
                printf("  TPM2 Support: %s%s%s\n",
                       FLAGS_SET(s, TPM2_SUPPORT_FIRMWARE|TPM2_SUPPORT_DRIVER) ? ansi_highlight_green() :
                       (s & (TPM2_SUPPORT_FIRMWARE|TPM2_SUPPORT_DRIVER)) != 0 ? ansi_highlight_red() : ansi_highlight_yellow(),
                       FLAGS_SET(s, TPM2_SUPPORT_FIRMWARE|TPM2_SUPPORT_DRIVER) ? "yes" :
                       (s & TPM2_SUPPORT_FIRMWARE) ? "firmware only, driver unavailable" :
                       (s & TPM2_SUPPORT_DRIVER) ? "driver only, firmware unavailable" : "no",
                       ansi_normal());

                k = efi_get_reboot_to_firmware();
                if (k > 0)
                        printf("  Boot into FW: %sactive%s\n", ansi_highlight_yellow(), ansi_normal());
                else if (k == 0)
                        printf("  Boot into FW: supported\n");
                else if (k == -EOPNOTSUPP)
                        printf("  Boot into FW: not supported\n");
                else {
                        errno = -k;
                        printf("  Boot into FW: %sfailed%s (%m)\n", ansi_highlight_red(), ansi_normal());
                }
                printf("\n");

                printf("%sCurrent Boot Loader:%s\n", ansi_underline(), ansi_normal());
                printf("      Product: %s%s%s\n", ansi_highlight(), strna(loader), ansi_normal());

                for (size_t i = 0; i < ELEMENTSOF(loader_flags); i++)
                        print_yes_no_line(i == 0, FLAGS_SET(loader_features, loader_flags[i].flag), loader_flags[i].name);

                sd_id128_t bootloader_esp_uuid;
                bool have_bootloader_esp_uuid = efi_loader_get_device_part_uuid(&bootloader_esp_uuid) >= 0;

                print_yes_no_line(false, have_bootloader_esp_uuid, "Boot loader sets ESP information");
                if (have_bootloader_esp_uuid && !sd_id128_is_null(esp_uuid) &&
                    !sd_id128_equal(esp_uuid, bootloader_esp_uuid))
                        printf("WARNING: The boot loader reports a different ESP UUID than detected ("SD_ID128_UUID_FORMAT_STR" vs. "SD_ID128_UUID_FORMAT_STR")!\n",
                               SD_ID128_FORMAT_VAL(bootloader_esp_uuid),
                               SD_ID128_FORMAT_VAL(esp_uuid));

                if (stub) {
                        printf("         Stub: %s\n", stub);
                        for (size_t i = 0; i < ELEMENTSOF(stub_flags); i++)
                                print_yes_no_line(i == 0, FLAGS_SET(stub_features, stub_flags[i].flag), stub_flags[i].name);
                }
                if (!sd_id128_is_null(loader_part_uuid))
                        printf("          ESP: /dev/disk/by-partuuid/" SD_ID128_UUID_FORMAT_STR "\n",
                               SD_ID128_FORMAT_VAL(loader_part_uuid));
                else
                        printf("          ESP: n/a\n");
                printf("         File: %s%s\n", special_glyph(SPECIAL_GLYPH_TREE_RIGHT), strna(loader_path));
                printf("\n");

                printf("%sRandom Seed:%s\n", ansi_underline(), ansi_normal());
                have = access(EFIVAR_PATH(EFI_LOADER_VARIABLE(LoaderSystemToken)), F_OK) >= 0;
                printf(" System Token: %s\n", have ? "set" : "not set");

                if (arg_esp_path) {
                        _cleanup_free_ char *p = NULL;

                        p = path_join(arg_esp_path, "/loader/random-seed");
                        if (!p)
                                return log_oom();

                        have = access(p, F_OK) >= 0;
                        printf("       Exists: %s\n", yes_no(have));
                }

                printf("\n");
        } else
                printf("%sSystem:%s\n"
                       "Not booted with EFI\n\n",
                       ansi_underline(), ansi_normal());

        if (arg_esp_path) {
                k = status_binaries(arg_esp_path, esp_uuid);
                if (k < 0)
                        r = k;
        }

        if (!arg_root && is_efi_boot()) {
                k = status_variables();
                if (k < 0)
                        r = k;
        }

        if (arg_esp_path || arg_xbootldr_path) {
                _cleanup_(boot_config_free) BootConfig config = BOOT_CONFIG_NULL;

                k = boot_config_load_and_select(&config,
                                                arg_esp_path, esp_devid,
                                                arg_xbootldr_path, xbootldr_devid);
                if (k < 0)
                        r = k;
                else {
                        k = status_entries(&config,
                                           arg_esp_path, esp_uuid,
                                           arg_xbootldr_path, xbootldr_uuid);
                        if (k < 0)
                                r = k;
                }
        }

        return r;
}

static int ref_file(Hashmap *known_files, const char *fn, const char *root)
{
        const char *p, *k = NULL;
        int r, n;

        assert(known_files);
        if (!fn)
                return -EINVAL;

        p = prefix_roota(root, fn);

        n = PTR_TO_INT(hashmap_get2(known_files, p, (void**)&k));
        if (!k)
                r = hashmap_put(known_files, strdup(p), INT_TO_PTR(1));
        else
                r = hashmap_update(known_files, p, INT_TO_PTR(++n));

        if (r < 0)
                return log_error_errno(r, "Failed to add %s to hashmap: %m", p);

        return n;
}

static int deref_file(Hashmap *known_files, const char *fn, const char *root)
{
        const char *p;
        int n;

        assert(known_files);
        if (!fn)
                return -EINVAL;

        p = prefix_roota(root, fn);

        n = PTR_TO_INT(hashmap_get(known_files, p));
        if (n > 0) {
                int r = hashmap_update(known_files, p, INT_TO_PTR(--n));
                if (r < 0)
                        return log_error_errno(r, "failed to update %s", p);
        }
        /* don't remove key even if count is zero as that would
         * produce a memory leak: There is no API to free the key.
         * Ony the destructor can do that. */

        return n;
}

static int deref_unlink_file(Hashmap *known_files, const char *fn, const char *root)
{
        _cleanup_free_ char *path = NULL;
        int r;
        r = deref_file(known_files, fn, root);
        if (r == 0) {
                r = chase_symlinks_and_access(fn, root, CHASE_PREFIX_ROOT|CHASE_PROHIBIT_SYMLINKS, F_OK, &path, NULL);
                if (r < 0)
                        return r;
                if (!arg_dry && unlink(path) < 0)
                        return log_error_errno(errno, "failed to remove \"%s\": %m", path);
                log_info("Removed %s", path);
        }
        return r;
}

static int count_known_files(const BootConfig *config, Hashmap **known_files)
{
        assert(config);
        assert(known_files);

        *known_files = hashmap_new(&string_hash_ops);
        if (!*known_files)
                return log_oom();

        for (size_t i = 0; i < config->n_entries; i++) {
                const BootEntry *e = config->entries + i;
                ref_file(*known_files, e->kernel, e->root);
                STRV_FOREACH(s, e->initrd)
                        ref_file(*known_files, *s, e->root);
                ref_file(*known_files, e->device_tree, e->root);
                STRV_FOREACH(s, e->device_tree_overlay)
                        ref_file(*known_files, *s, e->root);
        }

        return 0;
}

static int purge_entry(const BootConfig *config, const char *fn)
{
        _cleanup_(hashmap_free_free_keyp) Hashmap *known_files = NULL;
        _cleanup_free_ char *d = NULL;
        const BootEntry *e = NULL;
        int r;

        r = count_known_files(config, &known_files);
        if (r < 0)
                return log_error_errno(r, "failed to count files");

        for (size_t i = 0; i < config->n_entries; i++) {
                if (fnmatch(fn, config->entries[i].id, FNM_CASEFOLD))
                        continue;

                e = &config->entries[i];
                if (i == (size_t) config->default_entry)
                        log_warning("%s is the default boot entry", fn);
                if (i == (size_t) config->selected_entry)
                        log_warning("%s is the selected boot entry", fn);
                break;
        }

        if (!e)
                return log_error_errno(-ENOENT, "config %s not found", fn);

        // only process entries that boot something
        if (!e->kernel)
                return 0;

        deref_unlink_file(known_files, e->kernel, e->root);
        STRV_FOREACH(s, e->initrd)
                deref_unlink_file(known_files, *s, e->root);
        deref_unlink_file(known_files, e->device_tree, e->root);
        STRV_FOREACH(s, e->device_tree_overlay)
                deref_unlink_file(known_files, *s, e->root);

        path_extract_directory(e->kernel, &d);
        if (dir_is_empty(d, /* ignore_hidden_or_backup= */ false))
                rmdir(d);

        if (!arg_dry) {
                r = unlink(e->path);
                if (r < 0)
                        return log_error_errno(errno, "failed to remove \"%s\": %m", e->path);
        }

        log_info("Removed %s", e->path);

        return 0;
}

static int list_remove_orphaned_file(
                RecurseDirEvent event,
                const char *path,
                int dir_fd,
                int inode_fd,
                const struct dirent *de,
                const struct statx *sx,
                void *userdata) {

        Hashmap* known_files = userdata;

        assert(path);
        assert(known_files);

        if (event == RECURSE_DIR_ENTRY) {
                if (hashmap_get(known_files, path) == NULL) {
                        log_info("removing orphaned file %s\n", path);
                        if (!arg_dry) {
                                int r = unlink(path);
                                if (r < 0)
                                        log_error_errno(r, "failed to remove \"%s\": %m", path);
                                else
                                        log_info("Removed %s", path);
                        }
                }
        }

        return RECURSE_DIR_CONTINUE;
}

static int cleanup_orphaned_files(
                const BootConfig *config,
                const char *root) {
        _cleanup_(hashmap_free_free_keyp) Hashmap *known_files = NULL;
        _cleanup_free_ char *full = NULL;
        _cleanup_close_ int dir_fd = -1;
        int r = -1;

        assert(config);
        assert(root);

        r = settle_entry_token();
        if (r < 0)
                return r;

        r = count_known_files(config, &known_files);
        if (r < 0)
                return log_error_errno(r, "failed to count files");

        dir_fd = chase_symlinks_and_open(arg_entry_token, root, CHASE_PREFIX_ROOT|CHASE_PROHIBIT_SYMLINKS,
                        O_DIRECTORY|O_CLOEXEC, &full);
        if (dir_fd == -ENOENT)
                return 0;
        if (dir_fd < 0)
                return log_error_errno(dir_fd, "Failed to open '%s/%s': %m", root, arg_entry_token);

        r = recurse_dir(dir_fd, full, 0, UINT_MAX, RECURSE_DIR_SORT, list_remove_orphaned_file, known_files);

        return r;
}

int verb_list(int argc, char *argv[], void *userdata) {
        _cleanup_(boot_config_free) BootConfig config = BOOT_CONFIG_NULL;
        dev_t esp_devid = 0, xbootldr_devid = 0;
        int r;

        /* If we lack privileges we invoke find_esp_and_warn() in "unprivileged mode" here, which does two
         * things: turn off logging about access errors and turn off potentially privileged device probing.
         * Here we're interested in the latter but not the former, hence request the mode, and log about
         * EACCES. */

        r = acquire_esp(/* unprivileged_mode= */ geteuid() != 0, /* graceful= */ false, NULL, NULL, NULL, NULL, &esp_devid);
        if (r == -EACCES) /* We really need the ESP path for this call, hence also log about access errors */
                return log_error_errno(r, "Failed to determine ESP location: %m");
        if (r < 0)
                return r;

        r = acquire_xbootldr(/* unprivileged_mode= */ geteuid() != 0, NULL, &xbootldr_devid);
        if (r == -EACCES)
                return log_error_errno(r, "Failed to determine XBOOTLDR partition: %m");
        if (r < 0)
                return r;

        r = boot_config_load_and_select(&config, arg_esp_path, esp_devid, arg_xbootldr_path, xbootldr_devid);
        if (r < 0)
                return r;

        if (config.n_entries == 0 && FLAGS_SET(arg_json_format_flags, JSON_FORMAT_OFF)) {
                log_info("No boot loader entries found.");
                return 0;
        }

        if (streq(argv[0], "list")) {
                pager_open(arg_pager_flags);
                return show_boot_entries(&config, arg_json_format_flags);
        } else if (streq(argv[0], "cleanup")) {
                if (arg_xbootldr_path && xbootldr_devid != esp_devid)
                        cleanup_orphaned_files(&config, arg_xbootldr_path);
                return cleanup_orphaned_files(&config, arg_esp_path);
        } else
                return purge_entry(&config, argv[1]);
}

int verb_purge_entry(int argc, char *argv[], void *userdata) {
        return verb_list(argc, argv, userdata);
}
