/* SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * Copyright © 2024 GNOME Foundation Inc.
 *      Original Author: Adrian Vovk
 */

#include "copy.h"
#include "fileio.h"
#include "fd-util.h"
#include "fs-util.h"
#include "home-util.h"
#include "homework-blob.h"
#include "homework.h"
#include "install-file.h"
#include "macro.h"
#include "path-util.h"
#include "recurse-dir.h"
#include "rm-rf.h"
#include "sha256.h"
#include "string-util.h"
#include "tmpfile-util.h"
#include "umask-util.h"
#include "utf8.h"

static int copy_one_blob(
                int src_fd,
                int dest_dfd,
                const char *name,
                uint64_t *total_size,
                uid_t uid,
                Hashmap *manifest) {
        _cleanup_close_ int dest = -EBADF;
        uint8_t hash[SHA256_DIGEST_SIZE], *known_hash;
        off_t initial, size;
        struct stat st;
        int r;

        assert(src_fd >= 0);
        assert(dest_dfd >= 0);
        assert(name);
        assert(total_size);
        assert(uid_is_valid(uid));
        assert(manifest);

        if (fstat(src_fd, &st) < 0)
                return log_debug_errno(errno, "Failed to stat fd for %s in blob: %m", name);

        if (S_ISDIR(st.st_mode)) {
                log_warning("Entry %s in blob directory is a directory. Skipping.", name);
                return 0;
        } else if (!S_ISREG(st.st_mode)) {
                log_warning("Entry %s in blob directory is not a regular file. Skipping.", name);
                return 0;
        }

        known_hash = hashmap_get(manifest, name);
        if (!known_hash) {
                log_warning("File %s in blob directory is missing from manifest. Skipping.", name);
                return 0;
        }

        if (!suitable_blob_filename(name)) {
                log_warning("File %s in blob directory has invalid filename. Skipping.", name);
                return 0;
        }

        initial = lseek(src_fd, 0, SEEK_CUR);
        if (initial < 0)
                return log_debug_errno(errno, "Failed to get initial pos on fd for %s in blob: %m", name);

        r = sha256_fd(src_fd, BLOB_DIR_MAX_SIZE, hash);
        if (r == -EFBIG)
                return log_warning_errno(SYNTHETIC_ERRNO(EOVERFLOW),
                                         "Blob directory has exceeded its size limit. Not copying any further.");
        if (r < 0)
                return log_debug_errno(r, "Failed to compute sha256 for %s in blob: %m", name);

        size = lseek(src_fd, 0, SEEK_CUR);
        if (size < 0)
                return log_debug_errno(errno, "Failed to get final pos on fd for %s in blob: %m", name);
        if (!DEC_SAFE(&size, initial))
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid seek position on fd for %s. Couldn't get size.", name);

        if (lseek(src_fd, initial, SEEK_SET) < 0)
                return log_debug_errno(errno, "Failed to rewind fd for %s in blob: %m", name);

        if (memcmp(hash, known_hash, SHA256_DIGEST_SIZE) != 0) {
                log_warning("File %s in blob directory has incorrect hash. Skipping.", name);
                return 0;
        }

        if (!INC_SAFE(total_size, size))
                *total_size = UINT64_MAX;
        if (*total_size > BLOB_DIR_MAX_SIZE)
                return log_warning_errno(SYNTHETIC_ERRNO(EOVERFLOW),
                                         "Blob directory has exceeded its size limit. Not copying any further.");

        WITH_UMASK(0000) {
                dest = openat(dest_dfd, name, O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW, 0644);
                if (dest < 0)
                        return log_debug_errno(errno, "Failed to create/open %s in dest blob: %m", name);
        }

        r = copy_bytes(src_fd, dest, BLOB_DIR_MAX_SIZE, 0);
        if (r < 0)
                return r;

        if (fchown(dest, uid, uid) < 0)
                return log_debug_errno(errno, "Failed to chown %s in dest blob: %m", name);

        return 0;
}

static int replace_blob_at(
                int src_base_dfd,
                const char *src_name,
                int dest_base_dfd,
                const char *dest_name,
                Hashmap *manifest,
                mode_t mode,
                uid_t uid) {
        _cleanup_free_ char *fn = NULL;
        _cleanup_close_ int src_dfd = -EBADF, dest_dfd = -EBADF;
        _cleanup_free_ DirectoryEntries *de = NULL;
        uint64_t total_size = 0;
        int r;

        assert(src_base_dfd >= 0);
        assert(src_name);
        assert(dest_base_dfd >= 0);
        assert(dest_name);
        assert(uid_is_valid(uid));

        src_dfd = openat(src_base_dfd, src_name, O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if (src_dfd < 0) {
                if (errno == ENOENT)
                        return 0;
                return log_debug_errno(errno, "Failed to open src blob dir: %m");
        }

        r = tempfn_random(dest_name, NULL, &fn);
        if (r < 0)
                return r;

        dest_dfd = open_mkdir_at(dest_base_dfd, fn, O_EXCL|O_CLOEXEC, mode);
        if (dest_dfd < 0)
                return log_debug_errno(dest_dfd, "Failed to create/open dest blob dir: %m");

        r = readdir_all(src_dfd, RECURSE_DIR_SORT, &de);
        if (r < 0) {
                log_debug_errno(r, "Failed to read src blob dir: %m");
                goto fail;
        }
        for (size_t i = 0; i < de->n_entries; i++) {
                const char *name = de->entries[i]->d_name;
                _cleanup_close_ int src_fd = -EBADF;

                src_fd = openat(src_dfd, name, O_RDONLY|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW);
                if (src_fd < 0) {
                        r = log_debug_errno(errno, "Failed to open %s in src blob dir: %m", name);
                        goto fail;
                }

                r = copy_one_blob(src_fd, dest_dfd, name, &total_size, uid, manifest);
                if (r == -EOVERFLOW)
                        break;
                if (r < 0)
                        goto fail;
        }

        if (fchown(dest_dfd, uid, uid) < 0) {
                r = log_debug_errno(errno, "Failed to chown dest blob dir: %m");
                goto fail;
        }

        r = install_file(dest_base_dfd, fn, dest_base_dfd, dest_name, INSTALL_REPLACE);
        if (r < 0) {
                r = log_debug_errno(r, "Failed to move dest blob dir into place: %m");
                goto fail;
        }

        return 0;

fail:
        (void) rm_rf_at(dest_base_dfd, fn, REMOVE_ROOT|REMOVE_PHYSICAL|REMOVE_MISSING_OK);
        return r;
}

int home_reconcile_blob_dirs(UserRecord *h, int root_fd, int reconciled) {
        _cleanup_close_ int sys_base_dfd = -EBADF;
        int r;

        assert(h);
        assert(root_fd >= 0);
        assert(reconciled >= 0);

        if (reconciled == USER_RECONCILE_IDENTICAL)
                return 0;

        sys_base_dfd = open(home_system_blob_dir(), O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if (sys_base_dfd < 0)
                return log_error_errno(errno, "Failed to open system blob dir: %m");

        if (reconciled == USER_RECONCILE_HOST_WON) {
                r = replace_blob_at(sys_base_dfd, h->user_name, root_fd, ".identity-blob",
                                    h->blob_manifest, 0700, h->uid);
                if (r < 0)
                        return log_error_errno(r, "Failed to replace embedded blob with system blob: %m");

                log_info("Replaced embedded blob dir with contents of system blob dir.");
        } else {
                assert(reconciled == USER_RECONCILE_EMBEDDED_WON);

                r = replace_blob_at(root_fd, ".identity-blob", sys_base_dfd, h->user_name,
                                    h->blob_manifest, 0755, 0);
                if (r < 0)
                        return log_error_errno(r, "Failed to replace system blob with embedded blob: %m");

                log_info("Replaced system blob dir with contents of embedded blob dir.");
        }
        return 0;
}

int home_apply_new_blob_dir(UserRecord *h, Hashmap *blobs) {
        _cleanup_free_ char *fn = NULL;
        _cleanup_close_ int base_dfd = -EBADF, dfd = -EBADF;
        uint64_t total_size;
        const char *filename;
        const void *v;
        int r;

        assert(h);

        if (!blobs) /* Shortcut: If no blobs are passed from dbus, we have nothing to do. */
                return 0;

        base_dfd = open(home_system_blob_dir(), O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if (base_dfd < 0)
                return log_error_errno(errno, "Failed to open system blob base dir: %m");

        if (hashmap_isempty(blobs)) {
                /* Shortcut: If blobs was passed but empty, we can simply delete the contents
                 * of the directory. */
                r = rm_rf_at(base_dfd, h->user_name, REMOVE_PHYSICAL|REMOVE_MISSING_OK);
                if (r < 0)
                        return log_error_errno(errno, "Failed to empty out system blob dir: %m");
                return 0;
        }

        r = tempfn_random(h->user_name, NULL, &fn);
        if (r < 0)
                return r;

        dfd = open_mkdir_at(base_dfd, fn, O_EXCL|O_CLOEXEC, 0755);
        if (dfd < 0)
                return log_error_errno(errno, "Failed to create system blob dir: %m");

        HASHMAP_FOREACH_KEY(v, filename, blobs) {
                r = copy_one_blob(PTR_TO_FD(v), dfd, filename, &total_size, 0, h->blob_manifest);
                if (r == -EOVERFLOW)
                        break;
                if (r < 0)
                        goto fail;
        }

        r = install_file(base_dfd, fn, base_dfd, h->user_name, INSTALL_REPLACE);
        if (r < 0) {
                r = log_error_errno(r, "Failed to move system blob dir into place: %m");
                goto fail;
        }

        log_info("Replaced system blob directory.");
        return 0;

fail:
        (void) rm_rf_at(base_dfd, fn, REMOVE_ROOT|REMOVE_PHYSICAL|REMOVE_MISSING_OK);
        return r;
}
