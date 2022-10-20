/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/random.h>
#include <sys/ioctl.h>
#if USE_SYS_RANDOM_H
#  include <sys/random.h>
#endif
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include "sd-id128.h"

#include "alloc-util.h"
#include "fd-util.h"
#include "fs-util.h"
#include "io-util.h"
#include "log.h"
#include "main-func.h"
#include "missing_random.h"
#include "missing_syscall.h"
#include "mkdir.h"
#include "parse-argument.h"
#include "parse-util.h"
#include "pretty-print.h"
#include "random-util.h"
#include "string-table.h"
#include "string-util.h"
#include "strv.h"
#include "sync-util.h"
#include "sha256.h"
#include "terminal-util.h"
#include "util.h"
#include "xattr-util.h"

typedef enum SeedAction {
        ACTION_LOAD,
        ACTION_SAVE,
        _ACTION_MAX,
        _ACTION_INVALID = -EINVAL,
} SeedAction;

typedef enum CreditEntropy {
        CREDIT_ENTROPY_NO_WAY,
        CREDIT_ENTROPY_YES_PLEASE,
        CREDIT_ENTROPY_YES_FORCED,
} CreditEntropy;

static SeedAction arg_action = _ACTION_INVALID;

static CreditEntropy may_credit(int seed_fd) {
        _cleanup_free_ char *creditable = NULL;
        const char *e;
        int r;

        assert(seed_fd >= 0);

        e = getenv("SYSTEMD_RANDOM_SEED_CREDIT");
        if (!e) {
                log_debug("$SYSTEMD_RANDOM_SEED_CREDIT is not set, not crediting entropy.");
                return CREDIT_ENTROPY_NO_WAY;
        }
        if (streq(e, "force")) {
                log_debug("$SYSTEMD_RANDOM_SEED_CREDIT is set to 'force', crediting entropy.");
                return CREDIT_ENTROPY_YES_FORCED;
        }

        r = parse_boolean(e);
        if (r <= 0) {
                if (r < 0)
                        log_warning_errno(r, "Failed to parse $SYSTEMD_RANDOM_SEED_CREDIT, not crediting entropy: %m");
                else
                        log_debug("Crediting entropy is turned off via $SYSTEMD_RANDOM_SEED_CREDIT, not crediting entropy.");

                return CREDIT_ENTROPY_NO_WAY;
        }

        /* Determine if the file is marked as creditable */
        r = fgetxattr_malloc(seed_fd, "user.random-seed-creditable", &creditable);
        if (r < 0) {
                if (ERRNO_IS_XATTR_ABSENT(r))
                        log_debug_errno(r, "Seed file is not marked as creditable, not crediting.");
                else
                        log_warning_errno(r, "Failed to read extended attribute, ignoring: %m");

                return CREDIT_ENTROPY_NO_WAY;
        }

        r = parse_boolean(creditable);
        if (r <= 0) {
                if (r < 0)
                        log_warning_errno(r, "Failed to parse user.random-seed-creditable extended attribute, ignoring: %s", creditable);
                else
                        log_debug("Seed file is marked as not creditable, not crediting.");

                return CREDIT_ENTROPY_NO_WAY;
        }

        /* Don't credit the random seed if we are in first-boot mode, because we are supposed to start from
         * scratch. This is a safety precaution for cases where we people ship "golden" images with empty
         * /etc but populated /var that contains a random seed. */
        r = RET_NERRNO(access("/run/systemd/first-boot", F_OK));
        if (r >= 0) {
                log_debug("Not crediting entropy, since booted in first-boot mode.");
                return CREDIT_ENTROPY_NO_WAY;
        }
        if (r == -ENOENT)
                /* All is good, we are not in first-boot mode. */
                return CREDIT_ENTROPY_YES_PLEASE;

        log_warning_errno(errno, "Failed to check whether we are in first-boot mode, not crediting entropy: %m");
        return CREDIT_ENTROPY_NO_WAY;
}

static int random_seed_size(int seed_fd, size_t *ret_size) {
        struct stat st;

        assert(ret_size);

        if (fstat(seed_fd, &st) < 0)
                return log_error_errno(errno, "Failed to stat() seed file " RANDOM_SEED ": %m");

        /* If the seed file is larger than what the kernel expects, then honour the existing size and
         * save/restore as much as it says */

        *ret_size = CLAMP((uint64_t)st.st_size, random_pool_size(), RANDOM_POOL_SIZE_MAX);
        return 0;
}

static int help(int argc, char *argv[], void *userdata) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-random-seed", "8", &link);
        if (r < 0)
                return log_oom();

        printf("%1$s [OPTIONS...] COMMAND\n"
               "\n%5$sLoad and save the system random seed at boot and shutdown%6$s\n"
               "\n%3$sCommands:%4$s\n"
               "  load                Load a random seed saved on disk into the kernel entropy pool\n"
               "  save                Save a new random seed on disk\n"
               "\n%3$sOptions:%4$s\n"
               "  -h --help           Show this help\n"
               "     --version        Show package version\n"
               "\nSee the %2$s for details.\n",
               program_invocation_short_name,
               link,
               ansi_underline(),
               ansi_normal(),
               ansi_underline(),
               ansi_normal());

        return 0;
}

static const char* const seed_action_table[_ACTION_MAX] = {
        [ACTION_LOAD] = "load",
        [ACTION_SAVE] = "save",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP_FROM_STRING(seed_action, SeedAction);

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
        };

        static const struct option options[] = {
                { "help",    no_argument, NULL, 'h'         },
                { "version", no_argument, NULL, ARG_VERSION },
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hpx", options, NULL)) >= 0)
                switch (c) {
                case 'h':
                        return help(0, NULL, NULL);
                case ARG_VERSION:
                        return version();
                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (optind + 1 != argc)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "This program requires one argument.");

        arg_action = seed_action_from_string(argv[optind]);
        if (arg_action == _ACTION_INVALID)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Unknown action '%s'", argv[optind]);

        return 1;
}

static int run(int argc, char *argv[]) {
        bool read_seed_file, write_seed_file, synchronous, hashed_old_seed = false;
        _cleanup_close_ int seed_fd = -1, random_fd = -1;
        _cleanup_free_ void* buf = NULL;
        struct sha256_ctx hash_state;
        size_t buf_size;
        ssize_t k, l;
        int r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        umask(0022);

        r = mkdir_parents(RANDOM_SEED, 0755);
        if (r < 0)
                return log_error_errno(r, "Failed to create directory " RANDOM_SEED_DIR ": %m");

        /* When we load the seed we read it and write it to the device and then immediately update the saved
         * seed with new data, to make sure the next boot gets seeded differently. */

        switch (arg_action) {
        case ACTION_LOAD:
                seed_fd = open(RANDOM_SEED, O_RDWR|O_CLOEXEC|O_NOCTTY|O_CREAT, 0600);
                if (seed_fd < 0) {
                        int open_rw_error = -errno;

                        write_seed_file = false;

                        seed_fd = open(RANDOM_SEED, O_RDONLY|O_CLOEXEC|O_NOCTTY);
                        if (seed_fd < 0) {
                                bool missing = errno == ENOENT;

                                log_full_errno(missing ? LOG_DEBUG : LOG_ERR,
                                               open_rw_error, "Failed to open " RANDOM_SEED " for writing: %m");
                                r = log_full_errno(missing ? LOG_DEBUG : LOG_ERR,
                                                   errno, "Failed to open " RANDOM_SEED " for reading: %m");
                                return missing ? 0 : r;
                        }
                } else
                        write_seed_file = true;

                random_fd = open("/dev/urandom", O_RDWR|O_CLOEXEC|O_NOCTTY);
                if (random_fd < 0)
                        return log_error_errno(errno, "Failed to open /dev/urandom: %m");

                read_seed_file = true;
                synchronous = true; /* make this invocation a synchronous barrier for random pool initialization */
                break;

        case ACTION_SAVE:
                random_fd = open("/dev/urandom", O_RDONLY|O_CLOEXEC|O_NOCTTY);
                if (random_fd < 0)
                        return log_error_errno(errno, "Failed to open /dev/urandom: %m");

                seed_fd = open(RANDOM_SEED, O_WRONLY|O_CLOEXEC|O_NOCTTY|O_CREAT, 0600);
                if (seed_fd < 0)
                        return log_error_errno(errno, "Failed to open " RANDOM_SEED ": %m");

                read_seed_file = false;
                write_seed_file = true;
                synchronous = false;
                break;

        default:
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Unknown verb '%s'.", argv[1]);
        }

        r = random_seed_size(seed_fd, &buf_size);
        if (r < 0)
                return r;

        buf = malloc(buf_size);
        if (!buf)
                return log_oom();

        if (read_seed_file) {
                sd_id128_t mid;

                /* First, let's write the machine ID into /dev/urandom, not crediting entropy. Why? As an
                 * extra protection against "golden images" that are put together sloppily, i.e. images which
                 * are duplicated on multiple systems but where the random seed file is not properly
                 * reset. Frequently the machine ID is properly reset on those systems however (simply
                 * because it's easier to notice, if it isn't due to address clashes and so on, while random
                 * seed equivalence is generally not noticed easily), hence let's simply write the machined
                 * ID into the random pool too. */
                r = sd_id128_get_machine(&mid);
                if (r < 0)
                        log_debug_errno(r, "Failed to get machine ID, ignoring: %m");
                else {
                        r = random_write_entropy(random_fd, &mid, sizeof(mid), false);
                        if (r < 0)
                                log_debug_errno(r, "Failed to write machine ID to /dev/urandom, ignoring: %m");
                }

                k = loop_read(seed_fd, buf, buf_size, false);
                if (k < 0)
                        log_error_errno(k, "Failed to read seed from " RANDOM_SEED ": %m");
                else if (k == 0)
                        log_debug("Seed file " RANDOM_SEED " not yet initialized, proceeding.");
                else {
                        CreditEntropy lets_credit;

                        /* If we're going to later write out a seed file, initialize a hash state with
                         * the contents of the seed file we just read, so that the new one can't regress
                         * in entropy. */
                        if (write_seed_file) {
                                sha256_init_ctx(&hash_state);
                                sha256_process_bytes(&k, sizeof(k), &hash_state); /* Hash length to distinguish from new seed. */
                                sha256_process_bytes(buf, k, &hash_state);
                                hashed_old_seed = true;
                        }

                        (void) lseek(seed_fd, 0, SEEK_SET);

                        lets_credit = may_credit(seed_fd);

                        /* Before we credit or use the entropy, let's make sure to securely drop the
                         * creditable xattr from the file, so that we never credit the same random seed
                         * again. Note that further down we'll write a new seed again, and likely mark it as
                         * credible again, hence this is just paranoia to close the short time window between
                         * the time we upload the random seed into the kernel and download the new one from
                         * it. */

                        if (fremovexattr(seed_fd, "user.random-seed-creditable") < 0) {
                                if (!ERRNO_IS_XATTR_ABSENT(errno))
                                        log_warning_errno(errno, "Failed to remove extended attribute, ignoring: %m");

                                /* Otherwise, there was no creditable flag set, which is OK. */
                        } else {
                                r = fsync_full(seed_fd);
                                if (r < 0) {
                                        log_warning_errno(r, "Failed to synchronize seed to disk, not crediting entropy: %m");

                                        if (lets_credit == CREDIT_ENTROPY_YES_PLEASE)
                                                lets_credit = CREDIT_ENTROPY_NO_WAY;
                                }
                        }

                        r = random_write_entropy(random_fd, buf, k,
                                                 IN_SET(lets_credit, CREDIT_ENTROPY_YES_PLEASE, CREDIT_ENTROPY_YES_FORCED));
                        if (r < 0)
                                log_error_errno(r, "Failed to write seed to /dev/urandom: %m");
                }
        }

        if (write_seed_file) {
                bool getrandom_worked = false;

                /* This is just a safety measure. Given that we are root and most likely created the file
                 * ourselves the mode and owner should be correct anyway. */
                r = fchmod_and_chown(seed_fd, 0600, 0, 0);
                if (r < 0)
                        return log_error_errno(r, "Failed to adjust seed file ownership and access mode: %m");

                /* Let's make this whole job asynchronous, i.e. let's make ourselves a barrier for
                 * proper initialization of the random pool. */
                k = getrandom(buf, buf_size, GRND_NONBLOCK);
                if (k < 0 && errno == EAGAIN && synchronous) {
                        log_notice("Kernel entropy pool is not initialized yet, waiting until it is.");
                        k = getrandom(buf, buf_size, 0); /* retry synchronously */
                }
                if (k < 0)
                        log_debug_errno(errno, "Failed to read random data with getrandom(), falling back to /dev/urandom: %m");
                else if ((size_t) k < buf_size)
                        log_debug("Short read from getrandom(), falling back to /dev/urandom.");
                else
                        getrandom_worked = true;

                if (!getrandom_worked) {
                        /* Retry with classic /dev/urandom */
                        k = loop_read(random_fd, buf, buf_size, false);
                        if (k < 0)
                                return log_error_errno(k, "Failed to read new seed from /dev/urandom: %m");
                        if (k == 0)
                                return log_error_errno(SYNTHETIC_ERRNO(EIO),
                                                       "Got EOF while reading from /dev/urandom.");
                }

                /* If we previously read in a seed file, then hash the new seed into the old one,
                 * and replace the last 32 bytes of the seed with the hash output, so that the
                 * new seed file can't regress in entropy. */
                if (hashed_old_seed) {
                        uint8_t hash[SHA256_DIGEST_SIZE];
                        sha256_process_bytes(&k, sizeof(k), &hash_state); /* Hash length to distinguish from old seed. */
                        sha256_process_bytes(buf, k, &hash_state);
                        sha256_finish_ctx(&hash_state, hash);
                        l = MIN((size_t)k, sizeof(hash));
                        memcpy((uint8_t *)buf + k - l, hash, l);
                }

                r = loop_write(seed_fd, buf, (size_t) k, false);
                if (r < 0)
                        return log_error_errno(r, "Failed to write new random seed file: %m");

                if (ftruncate(seed_fd, k) < 0)
                        return log_error_errno(r, "Failed to truncate random seed file: %m");

                r = fsync_full(seed_fd);
                if (r < 0)
                        return log_error_errno(r, "Failed to synchronize seed file: %m");

                /* If we got this random seed data from getrandom() the data is suitable for crediting
                 * entropy later on. Let's keep that in mind by setting an extended attribute. on the file */
                if (getrandom_worked)
                        if (fsetxattr(seed_fd, "user.random-seed-creditable", "1", 1, 0) < 0)
                                log_full_errno(ERRNO_IS_NOT_SUPPORTED(errno) ? LOG_DEBUG : LOG_WARNING, errno,
                                               "Failed to mark seed file as creditable, ignoring: %m");
        }

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
