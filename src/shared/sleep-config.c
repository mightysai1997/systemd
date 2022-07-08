/* SPDX-License-Identifier: LGPL-2.1-or-later */
/***
  Copyright © 2018 Dell Inc.
***/

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include "alloc-util.h"
#include "blockdev-util.h"
#include "btrfs-util.h"
#include "conf-parser.h"
#include "def.h"
#include "devnum-util.h"
#include "env-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "fileio.h"
#include "hexdecoct.h"
#include "id128-util.h"
#include "log.h"
#include "macro.h"
#include "path-util.h"
#include "sha256.h"
#include "sleep-config.h"
#include "stat-util.h"
#include "stdio-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "time-util.h"

int parse_sleep_config(SleepConfig **ret_sleep_config) {
        _cleanup_(free_sleep_configp) SleepConfig *sc = NULL;
        int allow_suspend = -1, allow_hibernate = -1,
            allow_s2h = -1, allow_hybrid_sleep = -1;

        sc = new0(SleepConfig, 1);
        if (!sc)
                return log_oom();

        const ConfigTableItem items[] = {
                { "Sleep", "AllowSuspend",              config_parse_tristate, 0, &allow_suspend                  },
                { "Sleep", "AllowHibernation",          config_parse_tristate, 0, &allow_hibernate                },
                { "Sleep", "AllowSuspendThenHibernate", config_parse_tristate, 0, &allow_s2h                      },
                { "Sleep", "AllowHybridSleep",          config_parse_tristate, 0, &allow_hybrid_sleep             },

                { "Sleep", "SuspendMode",               config_parse_strv,     0, sc->modes + SLEEP_SUSPEND       },
                { "Sleep", "SuspendState",              config_parse_strv,     0, sc->states + SLEEP_SUSPEND      },
                { "Sleep", "HibernateMode",             config_parse_strv,     0, sc->modes + SLEEP_HIBERNATE     },
                { "Sleep", "HibernateState",            config_parse_strv,     0, sc->states + SLEEP_HIBERNATE    },
                { "Sleep", "HybridSleepMode",           config_parse_strv,     0, sc->modes + SLEEP_HYBRID_SLEEP  },
                { "Sleep", "HybridSleepState",          config_parse_strv,     0, sc->states + SLEEP_HYBRID_SLEEP },

                { "Sleep", "HibernateDelaySec",         config_parse_sec,      0, &sc->hibernate_delay_sec        },
                {}
        };

        (void) config_parse_many_nulstr(
                        PKGSYSCONFDIR "/sleep.conf",
                        CONF_PATHS_NULSTR("systemd/sleep.conf.d"),
                        "Sleep\0",
                        config_item_table_lookup, items,
                        CONFIG_PARSE_WARN,
                        NULL,
                        NULL);

        /* use default values unless set */
        sc->allow[SLEEP_SUSPEND] = allow_suspend != 0;
        sc->allow[SLEEP_HIBERNATE] = allow_hibernate != 0;
        sc->allow[SLEEP_HYBRID_SLEEP] = allow_hybrid_sleep >= 0 ? allow_hybrid_sleep
                : (allow_suspend != 0 && allow_hibernate != 0);
        sc->allow[SLEEP_SUSPEND_THEN_HIBERNATE] = allow_s2h >= 0 ? allow_s2h
                : (allow_suspend != 0 && allow_hibernate != 0);

        if (!sc->states[SLEEP_SUSPEND])
                sc->states[SLEEP_SUSPEND] = strv_new("mem", "standby", "freeze");
        if (!sc->modes[SLEEP_HIBERNATE])
                sc->modes[SLEEP_HIBERNATE] = strv_new("platform", "shutdown");
        if (!sc->states[SLEEP_HIBERNATE])
                sc->states[SLEEP_HIBERNATE] = strv_new("disk");
        if (!sc->modes[SLEEP_HYBRID_SLEEP])
                sc->modes[SLEEP_HYBRID_SLEEP] = strv_new("suspend", "platform", "shutdown");
        if (!sc->states[SLEEP_HYBRID_SLEEP])
                sc->states[SLEEP_HYBRID_SLEEP] = strv_new("disk");
        if (sc->hibernate_delay_sec == 0)
                sc->hibernate_delay_sec = 2 * USEC_PER_HOUR;

        /* ensure values set for all required fields */
        if (!sc->states[SLEEP_SUSPEND] || !sc->modes[SLEEP_HIBERNATE]
            || !sc->states[SLEEP_HIBERNATE] || !sc->modes[SLEEP_HYBRID_SLEEP] || !sc->states[SLEEP_HYBRID_SLEEP])
                return log_oom();

        *ret_sleep_config = TAKE_PTR(sc);

        return 0;
}

int battery_is_low(void) {
        int r;

        r = read_battery_capacity_percentage();
        if (r < 0)
               return r;

        if (r > 0 && r <= 5)
               return 1;
               /* If battery current capacity percentage is equal to or less than 5%.
                * We have not used battery capacity_level since value is set to full
                * or Normal in case acpi is not working properly. */
        log_debug("Battery is not low");
        return 0;
        /* Not low if greater than 5%. The error will be handled
         * in read_battery_capacity_percentage which returns value from 0-100 only. */
}

int read_battery_capacity_percentage(void) {
        _cleanup_free_ char *bat_cap = NULL;
        int battery_capacity, r;

        r = read_one_line_file("/sys/class/power_supply/BAT0/capacity", &bat_cap);
        if (r == -ENOENT)
               return log_debug_errno(r, "/sys/class/power_supply/BAT0/capacity is unavailable. No battery case: %m");
               /* Handling case when no battery is present. Exception here file is manually deleted / corrupted */
        else if (r < 0)
               return log_debug_errno(r, "Failed to read /sys/class/power_supply/BAT0/capacity: %m");

        r = safe_atoi(bat_cap, &battery_capacity);
        if (r < 0)
               return log_debug_errno(r, "Failed to parse battery capacity: %m");

        if (battery_capacity < 0 || battery_capacity > 100)
               return log_debug_errno(SYNTHETIC_ERRNO(ERANGE), "Invalid battery capacity");

        return battery_capacity;
}

static int get_system_battery_identifier_hash(uint8_t ret[static SHA256_DIGEST_SIZE]) {
        _cleanup_free_ char *bat_manufacturer = NULL, *bat_model_name = NULL, *bat_serial_number = NULL;
        sd_id128_t machine_id, product_id;
        struct sha256_ctx hash;
        char *h = NULL, *p = NULL;
        int r;

        r = read_one_line_file("/sys/class/power_supply/BAT0/manufacturer", &bat_manufacturer);
        if (r == -ENOENT)
               log_debug_errno(r, "/sys/class/power_supply/BAT0/manufacturer is unavailable: %m");
        else if (r < 0)
               return log_debug_errno(r, "Failed to read /sys/class/power_supply/BAT0/manufacturer: %m");

        r = read_one_line_file("/sys/class/power_supply/BAT0/model_name", &bat_model_name);
        if (r == -ENOENT)
               log_debug_errno(r, "/sys/class/power_supply/BAT0/model_name is unavailable: %m");
        else if (r < 0)
               return log_debug_errno(r, "Failed to read /sys/class/power_supply/BAT0/model_name: %m");

        r = read_one_line_file("/sys/class/power_supply/BAT0/serial_number", &bat_serial_number);
        if (r == -ENOENT)
               log_debug_errno(r, "/sys/class/power_supply/BAT0/serial_number is unavailable: %m");
        else if (r < 0)
               return log_debug_errno(r, "Failed to read /sys/class/power_supply/BAT0/serial_number: %m");

        r = sd_id128_get_machine(&machine_id);
        if (r < 0)
               return log_debug_errno(r, "Failed to get machine ID: %m");
        h = SD_ID128_TO_STRING(machine_id);

        r = id128_get_product(&product_id);
        if (r == -ENOENT)
               log_debug_errno(r, "product_id does not exist: %m");
        else if (r < 0)
                return log_debug_errno(r, "Failed to get product ID: %m");
        p = SD_ID128_TO_STRING(product_id);

        sha256_init_ctx(&hash);
        if(bat_manufacturer)
                sha256_process_bytes(bat_manufacturer, sizeof(bat_manufacturer), &hash);
        if(bat_model_name)
                sha256_process_bytes(bat_model_name, sizeof(bat_model_name), &hash);
        if(bat_serial_number)
                sha256_process_bytes(bat_serial_number, sizeof(bat_serial_number), &hash);
        if(h)
                sha256_process_bytes(h, sizeof(h), &hash);
        if(p)
                sha256_process_bytes(p, sizeof(p), &hash);
        sha256_finish_ctx(&hash, ret);

        return 0;
}

static int battery_discharge_rate_in_range(int battery_discharge_rate) {
        if (battery_discharge_rate <= 0 || battery_discharge_rate >= 200)
               return 0;
               /* battery discharge rate is not in a valid range */
        log_debug("Battery discharge rate is in valid range");
        return 1;
}

int get_battery_discharge_rate(void) {
        _cleanup_free_ char *hash_id_discharge_rate = NULL;
        const char *p, *filepath;
        uint8_t current_hash_id[SHA256_DIGEST_SIZE];
        char *stored_hash_id = NULL, *stored_discharge_rate = NULL, *h = NULL;
        int discharge_rate, r;

        filepath = "/var/lib/systemd/sleep/battery_discharge_percentage_rate_per_hour";

        r = read_one_line_file(filepath, &hash_id_discharge_rate);
        log_debug("Stored hash_id and discharge_rate is %s", hash_id_discharge_rate);
        if (r < 0)
               return log_debug_errno(r, "Failed to read discharge rate from %s: %m", filepath);

        r = get_system_battery_identifier_hash(current_hash_id);
        if (r < 0)
               return log_debug_errno(r, "Failed to generate system battery identifier hash: %m");

        p = hash_id_discharge_rate;
        r = extract_many_words(&p, " ", 0, &stored_hash_id, &stored_discharge_rate, NULL);
        log_debug("stored hash_id is %s and stored discharge_rate is %s", stored_hash_id, stored_discharge_rate);
        if (r < 0)
               return log_debug_errno(r, "Failed to parse hash_id and discharge_rate read from %s location: %m", filepath);

        h = hexmem(current_hash_id, sizeof(current_hash_id));
        if (!strcmp_ptr(h, stored_hash_id))
               return log_debug_errno(SYNTHETIC_ERRNO(ENOENT), "Current dentifier do not match stored identifier: %m");
        log_debug("hash id match");

        r = safe_atoi(stored_discharge_rate, &discharge_rate);
        if (r < 0)
               return log_debug_errno(r, "Failed to parse discharge rate read from %s location: %m", filepath);

        if (!battery_discharge_rate_in_range(discharge_rate))
               return log_debug_errno(SYNTHETIC_ERRNO(ERANGE), "Invalid battery discharge percentage rate per hour: %m");

        return discharge_rate;
}

int put_battery_discharge_rate(int estimated_battery_discharge_rate) {
        uint8_t *system_hash_id = NULL;
        const char *filepath;
        char *p = NULL, *h = NULL;
        int r = 0;

        if (!battery_discharge_rate_in_range(estimated_battery_discharge_rate))
               return log_debug_errno(SYNTHETIC_ERRNO(ERANGE), "Invalid battery discharge percentage rate per hour: %m");

        r = get_system_battery_identifier_hash(system_hash_id);
        if (r < 0)
               return log_debug_errno(r, "Failed to generate system battery identifier hash: %m");
        log_debug("system_hash_id is %s", system_hash_id);

        h = hexmem(system_hash_id, sizeof(system_hash_id));
        p = strjoin(h, " ", estimated_battery_discharge_rate);
        if (!p)
                return log_oom_debug();
        filepath = "/var/lib/systemd/sleep/battery_discharge_percentage_rate_per_hour";
        log_debug("Final String to be written in %s is %s", filepath, p);

        r = write_string_filef(filepath, WRITE_STRING_FILE_CREATE|WRITE_STRING_FILE_MKDIR_0755, "%s", p);
        if (r < 0)
                return log_debug_errno(r, "Failed to create %s: %m", filepath);

        return r;
}

int can_sleep_state(char **types) {
        _cleanup_free_ char *text = NULL;
        int r;

        if (strv_isempty(types))
                return true;

        /* If /sys is read-only we cannot sleep */
        if (access("/sys/power/state", W_OK) < 0) {
                log_debug_errno(errno, "/sys/power/state is not writable, cannot sleep: %m");
                return false;
        }

        r = read_one_line_file("/sys/power/state", &text);
        if (r < 0) {
                log_debug_errno(r, "Failed to read /sys/power/state, cannot sleep: %m");
                return false;
        }

        const char *found;
        r = string_contains_word_strv(text, NULL, types, &found);
        if (r < 0)
                return log_debug_errno(r, "Failed to parse /sys/power/state: %m");
        if (r > 0)
                log_debug("Sleep mode \"%s\" is supported by the kernel.", found);
        else if (DEBUG_LOGGING) {
                _cleanup_free_ char *t = strv_join(types, "/");
                log_debug("Sleep mode %s not supported by the kernel, sorry.", strnull(t));
        }
        return r;
}

int can_sleep_disk(char **types) {
        _cleanup_free_ char *text = NULL;
        int r;

        if (strv_isempty(types))
                return true;

        /* If /sys is read-only we cannot sleep */
        if (access("/sys/power/disk", W_OK) < 0) {
                log_debug_errno(errno, "/sys/power/disk is not writable: %m");
                return false;
        }

        r = read_one_line_file("/sys/power/disk", &text);
        if (r < 0) {
                log_debug_errno(r, "Couldn't read /sys/power/disk: %m");
                return false;
        }

        for (const char *p = text;;) {
                _cleanup_free_ char *word = NULL;

                r = extract_first_word(&p, &word, NULL, 0);
                if (r < 0)
                        return log_debug_errno(r, "Failed to parse /sys/power/disk: %m");
                if (r == 0)
                        break;

                char *s = word;
                size_t l = strlen(s);
                if (s[0] == '[' && s[l-1] == ']') {
                        s[l-1] = '\0';
                        s++;
                }

                if (strv_contains(types, s)) {
                        log_debug("Disk sleep mode \"%s\" is supported by the kernel.", s);
                        return true;
                }
        }

        if (DEBUG_LOGGING) {
                _cleanup_free_ char *t = strv_join(types, "/");
                log_debug("Disk sleep mode %s not supported by the kernel, sorry.", strnull(t));
        }
        return false;
}

#define HIBERNATION_SWAP_THRESHOLD 0.98

SwapEntry* swap_entry_free(SwapEntry *se) {
        if (!se)
                return NULL;

        free(se->device);
        free(se->type);

        return mfree(se);
}

HibernateLocation* hibernate_location_free(HibernateLocation *hl) {
        if (!hl)
                return NULL;

        swap_entry_free(hl->swap);

        return mfree(hl);
}

static int swap_device_to_device_id(const SwapEntry *swap, dev_t *ret_dev) {
        struct stat sb;
        int r;

        assert(swap);
        assert(swap->device);
        assert(swap->type);

        r = stat(swap->device, &sb);
        if (r < 0)
                return -errno;

        if (streq(swap->type, "partition")) {
                if (!S_ISBLK(sb.st_mode))
                        return -ENOTBLK;

                *ret_dev = sb.st_rdev;
                return 0;
        }

        return get_block_device(swap->device, ret_dev);
}

/*
 * Attempt to calculate the swap file offset on supported filesystems. On unsupported
 * filesystems, a debug message is logged and ret_offset is set to UINT64_MAX.
 */
static int calculate_swap_file_offset(const SwapEntry *swap, uint64_t *ret_offset) {
        _cleanup_close_ int fd = -1;
        _cleanup_free_ struct fiemap *fiemap = NULL;
        struct stat sb;
        int r;

        assert(swap);
        assert(swap->device);
        assert(streq(swap->type, "file"));

        fd = open(swap->device, O_RDONLY|O_CLOEXEC|O_NOCTTY);
        if (fd < 0)
                return log_debug_errno(errno, "Failed to open swap file %s to determine on-disk offset: %m", swap->device);

        if (fstat(fd, &sb) < 0)
                return log_debug_errno(errno, "Failed to stat %s: %m", swap->device);

        r = fd_is_fs_type(fd, BTRFS_SUPER_MAGIC);
        if (r < 0)
                return log_debug_errno(r, "Error checking %s for Btrfs filesystem: %m", swap->device);
        if (r > 0) {
                log_debug("%s: detection of swap file offset on Btrfs is not supported", swap->device);
                *ret_offset = UINT64_MAX;
                return 0;
        }

        r = read_fiemap(fd, &fiemap);
        if (r < 0)
                return log_debug_errno(r, "Unable to read extent map for '%s': %m", swap->device);

        *ret_offset = fiemap->fm_extents[0].fe_physical / page_size();
        return 0;
}

static int read_resume_files(dev_t *ret_resume, uint64_t *ret_resume_offset) {
        _cleanup_free_ char *resume_str = NULL, *resume_offset_str = NULL;
        uint64_t resume_offset = 0;
        dev_t resume;
        int r;

        r = read_one_line_file("/sys/power/resume", &resume_str);
        if (r < 0)
                return log_debug_errno(r, "Error reading /sys/power/resume: %m");

        r = parse_devnum(resume_str, &resume);
        if (r < 0)
                return log_debug_errno(r, "Error parsing /sys/power/resume device: %s: %m", resume_str);

        r = read_one_line_file("/sys/power/resume_offset", &resume_offset_str);
        if (r == -ENOENT)
                log_debug_errno(r, "Kernel does not support resume_offset; swap file offset detection will be skipped.");
        else if (r < 0)
                return log_debug_errno(r, "Error reading /sys/power/resume_offset: %m");
        else {
                r = safe_atou64(resume_offset_str, &resume_offset);
                if (r < 0)
                        return log_debug_errno(r, "Failed to parse value in /sys/power/resume_offset \"%s\": %m", resume_offset_str);
        }

        if (resume_offset > 0 && resume == 0)
                log_debug("Warning: found /sys/power/resume_offset==%" PRIu64 ", but /sys/power/resume unset. Misconfiguration?",
                          resume_offset);

        *ret_resume = resume;
        *ret_resume_offset = resume_offset;

        return 0;
}

/*
 * Determine if the HibernateLocation matches the resume= (device) and resume_offset= (file).
 */
static bool location_is_resume_device(const HibernateLocation *location, dev_t sys_resume, uint64_t sys_offset) {
        if (!location)
                return false;

        return  sys_resume > 0 &&
                sys_resume == location->devno &&
                (sys_offset == location->offset || (sys_offset > 0 && location->offset == UINT64_MAX));
}

/*
 * Attempt to find the hibernation location by parsing /proc/swaps, /sys/power/resume, and
 * /sys/power/resume_offset.
 *
 * Returns:
 *  1 - Values are set in /sys/power/resume and /sys/power/resume_offset.
 *      ret_hibernate_location will represent matching /proc/swap entry if identified or NULL if not.
 *
 *  0 - No values are set in /sys/power/resume and /sys/power/resume_offset.
        ret_hibernate_location will represent the highest priority swap with most remaining space discovered in /proc/swaps.
 *
 *  Negative value in the case of error.
 */
int find_hibernate_location(HibernateLocation **ret_hibernate_location) {
        _cleanup_fclose_ FILE *f = NULL;
        _cleanup_(hibernate_location_freep) HibernateLocation *hibernate_location = NULL;
        dev_t sys_resume = 0; /* Unnecessary initialization to appease gcc */
        uint64_t sys_offset = 0;
        bool resume_match = false;
        int r;

        /* read the /sys/power/resume & /sys/power/resume_offset values */
        r = read_resume_files(&sys_resume, &sys_offset);
        if (r < 0)
                return r;

        f = fopen("/proc/swaps", "re");
        if (!f) {
                log_debug_errno(errno, "Failed to open /proc/swaps: %m");
                return errno == ENOENT ? -EOPNOTSUPP : -errno; /* Convert swap not supported to a recognizable error */
        }

        (void) fscanf(f, "%*s %*s %*s %*s %*s\n");
        for (unsigned i = 1;; i++) {
                _cleanup_(swap_entry_freep) SwapEntry *swap = NULL;
                uint64_t swap_offset = 0;
                int k;

                swap = new0(SwapEntry, 1);
                if (!swap)
                        return -ENOMEM;

                k = fscanf(f,
                           "%ms "       /* device/file */
                           "%ms "       /* type of swap */
                           "%" PRIu64   /* swap size */
                           "%" PRIu64   /* used */
                           "%i\n",      /* priority */
                           &swap->device, &swap->type, &swap->size, &swap->used, &swap->priority);
                if (k == EOF)
                        break;
                if (k != 5) {
                        log_debug("Failed to parse /proc/swaps:%u, ignoring", i);
                        continue;
                }

                if (streq(swap->type, "file")) {
                        if (endswith(swap->device, "\\040(deleted)")) {
                                log_debug("Ignoring deleted swap file '%s'.", swap->device);
                                continue;
                        }

                        r = calculate_swap_file_offset(swap, &swap_offset);
                        if (r < 0)
                                return r;

                } else if (streq(swap->type, "partition")) {
                        const char *fn;

                        fn = path_startswith(swap->device, "/dev/");
                        if (fn && startswith(fn, "zram")) {
                                log_debug("%s: ignoring zram swap", swap->device);
                                continue;
                        }

                } else {
                        log_debug("%s: swap type %s is unsupported for hibernation, ignoring", swap->device, swap->type);
                        continue;
                }

                /* prefer resume device or highest priority swap with most remaining space */
                if (sys_resume == 0) {
                        if (hibernate_location && swap->priority < hibernate_location->swap->priority) {
                                log_debug("%s: ignoring device with lower priority", swap->device);
                                continue;
                        }
                        if (hibernate_location &&
                            (swap->priority == hibernate_location->swap->priority
                             && swap->size - swap->used < hibernate_location->swap->size - hibernate_location->swap->used)) {
                                log_debug("%s: ignoring device with lower usable space", swap->device);
                                continue;
                        }
                }

                dev_t swap_device;
                r = swap_device_to_device_id(swap, &swap_device);
                if (r < 0)
                        return log_debug_errno(r, "%s: failed to query device number: %m", swap->device);
                if (swap_device == 0)
                        return log_debug_errno(SYNTHETIC_ERRNO(ENODEV), "%s: not backed by block device.", swap->device);

                hibernate_location = hibernate_location_free(hibernate_location);
                hibernate_location = new(HibernateLocation, 1);
                if (!hibernate_location)
                        return -ENOMEM;

                *hibernate_location = (HibernateLocation) {
                        .devno = swap_device,
                        .offset = swap_offset,
                        .swap = TAKE_PTR(swap),
                };

                /* if the swap is the resume device, stop the loop */
                if (location_is_resume_device(hibernate_location, sys_resume, sys_offset)) {
                        log_debug("%s: device matches configured resume settings.", hibernate_location->swap->device);
                        resume_match = true;
                        break;
                }

                log_debug("%s: is a candidate device.", hibernate_location->swap->device);
        }

        /* We found nothing at all */
        if (!hibernate_location)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOSYS),
                                       "No possible swap partitions or files suitable for hibernation were found in /proc/swaps.");

        /* resume= is set but a matching /proc/swaps entry was not found */
        if (sys_resume != 0 && !resume_match)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOSYS),
                                       "No swap partitions or files matching resume config were found in /proc/swaps.");

        if (hibernate_location->offset == UINT64_MAX) {
                if (sys_offset == 0)
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOSYS), "Offset detection failed and /sys/power/resume_offset is not set.");

                hibernate_location->offset = sys_offset;
        }

        if (resume_match)
                log_debug("Hibernation will attempt to use swap entry with path: %s, device: %u:%u, offset: %" PRIu64 ", priority: %i",
                          hibernate_location->swap->device, major(hibernate_location->devno), minor(hibernate_location->devno),
                          hibernate_location->offset, hibernate_location->swap->priority);
        else
                log_debug("/sys/power/resume is not configured; attempting to hibernate with path: %s, device: %u:%u, offset: %" PRIu64 ", priority: %i",
                          hibernate_location->swap->device, major(hibernate_location->devno), minor(hibernate_location->devno),
                          hibernate_location->offset, hibernate_location->swap->priority);

        *ret_hibernate_location = TAKE_PTR(hibernate_location);

        if (resume_match)
                return 1;

        return 0;
}

static bool enough_swap_for_hibernation(void) {
        _cleanup_free_ char *active = NULL;
        _cleanup_(hibernate_location_freep) HibernateLocation *hibernate_location = NULL;
        unsigned long long act = 0;
        int r;

        if (getenv_bool("SYSTEMD_BYPASS_HIBERNATION_MEMORY_CHECK") > 0)
                return true;

        r = find_hibernate_location(&hibernate_location);
        if (r < 0)
                return false;

        /* If /sys/power/{resume,resume_offset} is configured but a matching entry
         * could not be identified in /proc/swaps, user is likely using Btrfs with a swapfile;
         * return true and let the system attempt hibernation.
         */
        if (r > 0 && !hibernate_location) {
                log_debug("Unable to determine remaining swap space; hibernation may fail");
                return true;
        }

        if (!hibernate_location)
                return false;

        r = get_proc_field("/proc/meminfo", "Active(anon)", WHITESPACE, &active);
        if (r < 0) {
                log_debug_errno(r, "Failed to retrieve Active(anon) from /proc/meminfo: %m");
                return false;
        }

        r = safe_atollu(active, &act);
        if (r < 0) {
                log_debug_errno(r, "Failed to parse Active(anon) from /proc/meminfo: %s: %m", active);
                return false;
        }

        r = act <= (hibernate_location->swap->size - hibernate_location->swap->used) * HIBERNATION_SWAP_THRESHOLD;
        log_debug("%s swap for hibernation, Active(anon)=%llu kB, size=%" PRIu64 " kB, used=%" PRIu64 " kB, threshold=%.2g%%",
                  r ? "Enough" : "Not enough", act, hibernate_location->swap->size, hibernate_location->swap->used, 100*HIBERNATION_SWAP_THRESHOLD);

        return r;
}

int read_fiemap(int fd, struct fiemap **ret) {
        _cleanup_free_ struct fiemap *fiemap = NULL, *result_fiemap = NULL;
        struct stat statinfo;
        uint32_t result_extents = 0;
        uint64_t fiemap_start = 0, fiemap_length;
        const size_t n_extra = DIV_ROUND_UP(sizeof(struct fiemap), sizeof(struct fiemap_extent));

        if (fstat(fd, &statinfo) < 0)
                return log_debug_errno(errno, "Cannot determine file size: %m");
        if (!S_ISREG(statinfo.st_mode))
                return -ENOTTY;
        fiemap_length = statinfo.st_size;

        /* Zero this out in case we run on a file with no extents */
        fiemap = calloc(n_extra, sizeof(struct fiemap_extent));
        if (!fiemap)
                return -ENOMEM;

        result_fiemap = malloc_multiply(n_extra, sizeof(struct fiemap_extent));
        if (!result_fiemap)
                return -ENOMEM;

        /*  XFS filesystem has incorrect implementation of fiemap ioctl and
         *  returns extents for only one block-group at a time, so we need
         *  to handle it manually, starting the next fiemap call from the end
         *  of the last extent
         */
        while (fiemap_start < fiemap_length) {
                *fiemap = (struct fiemap) {
                        .fm_start = fiemap_start,
                        .fm_length = fiemap_length,
                        .fm_flags = FIEMAP_FLAG_SYNC,
                };

                /* Find out how many extents there are */
                if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0)
                        return log_debug_errno(errno, "Failed to read extents: %m");

                /* Nothing to process */
                if (fiemap->fm_mapped_extents == 0)
                        break;

                /* Resize fiemap to allow us to read in the extents, result fiemap has to hold all
                 * the extents for the whole file. Add space for the initial struct fiemap. */
                if (!greedy_realloc0((void**) &fiemap, n_extra + fiemap->fm_mapped_extents, sizeof(struct fiemap_extent)))
                        return -ENOMEM;

                fiemap->fm_extent_count = fiemap->fm_mapped_extents;
                fiemap->fm_mapped_extents = 0;

                if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0)
                        return log_debug_errno(errno, "Failed to read extents: %m");

                /* Resize result_fiemap to allow us to copy in the extents */
                if (!greedy_realloc((void**) &result_fiemap,
                                    n_extra + result_extents + fiemap->fm_mapped_extents, sizeof(struct fiemap_extent)))
                        return -ENOMEM;

                memcpy(result_fiemap->fm_extents + result_extents,
                       fiemap->fm_extents,
                       sizeof(struct fiemap_extent) * fiemap->fm_mapped_extents);

                result_extents += fiemap->fm_mapped_extents;

                /* Highly unlikely that it is zero */
                if (_likely_(fiemap->fm_mapped_extents > 0)) {
                        uint32_t i = fiemap->fm_mapped_extents - 1;

                        fiemap_start = fiemap->fm_extents[i].fe_logical +
                                       fiemap->fm_extents[i].fe_length;

                        if (fiemap->fm_extents[i].fe_flags & FIEMAP_EXTENT_LAST)
                                break;
                }
        }

        memcpy(result_fiemap, fiemap, sizeof(struct fiemap));
        result_fiemap->fm_mapped_extents = result_extents;
        *ret = TAKE_PTR(result_fiemap);
        return 0;
}

static int can_sleep_internal(const SleepConfig *sleep_config, SleepOperation operation, bool check_allowed);

static bool can_s2h(const SleepConfig *sleep_config) {

        static const SleepOperation operations[] = {
                SLEEP_SUSPEND,
                SLEEP_HIBERNATE,
        };

        int r;

        if (!clock_supported(CLOCK_BOOTTIME_ALARM)) {
                log_debug("CLOCK_BOOTTIME_ALARM is not supported.");
                return false;
        }

        for (size_t i = 0; i < ELEMENTSOF(operations); i++) {
                r = can_sleep_internal(sleep_config, operations[i], false);
                if (IN_SET(r, 0, -ENOSPC)) {
                        log_debug("Unable to %s system.", sleep_operation_to_string(operations[i]));
                        return false;
                }
                if (r < 0)
                        return log_debug_errno(r, "Failed to check if %s is possible: %m", sleep_operation_to_string(operations[i]));
        }

        return true;
}

static int can_sleep_internal(
                const SleepConfig *sleep_config,
                SleepOperation operation,
                bool check_allowed) {

        assert(operation >= 0);
        assert(operation < _SLEEP_OPERATION_MAX);

        if (check_allowed && !sleep_config->allow[operation]) {
                log_debug("Sleep mode \"%s\" is disabled by configuration.", sleep_operation_to_string(operation));
                return false;
        }

        if (operation == SLEEP_SUSPEND_THEN_HIBERNATE)
                return can_s2h(sleep_config);

        if (can_sleep_state(sleep_config->states[operation]) <= 0 ||
            can_sleep_disk(sleep_config->modes[operation]) <= 0)
                return false;

        if (operation == SLEEP_SUSPEND)
                return true;

        if (!enough_swap_for_hibernation())
                return -ENOSPC;

        return true;
}

int can_sleep(SleepOperation operation) {
        _cleanup_(free_sleep_configp) SleepConfig *sleep_config = NULL;
        int r;

        r = parse_sleep_config(&sleep_config);
        if (r < 0)
                return r;

        return can_sleep_internal(sleep_config, operation, true);
}

SleepConfig* free_sleep_config(SleepConfig *sc) {
        if (!sc)
                return NULL;

        for (SleepOperation i = 0; i < _SLEEP_OPERATION_MAX; i++) {
                strv_free(sc->modes[i]);
                strv_free(sc->states[i]);
        }

        return mfree(sc);
}

static const char* const sleep_operation_table[_SLEEP_OPERATION_MAX] = {
        [SLEEP_SUSPEND]                = "suspend",
        [SLEEP_HIBERNATE]              = "hibernate",
        [SLEEP_HYBRID_SLEEP]           = "hybrid-sleep",
        [SLEEP_SUSPEND_THEN_HIBERNATE] = "suspend-then-hibernate",
};

DEFINE_STRING_TABLE_LOOKUP(sleep_operation, SleepOperation);
