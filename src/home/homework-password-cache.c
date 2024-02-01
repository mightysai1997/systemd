/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "homework-password-cache.h"
#include "keyring-util.h"
#include "missing_syscall.h"
#include "user-record.h"

void password_cache_free(PasswordCache *cache) {
        if (!cache)
                return;

        cache->volume_key = erase_and_free(cache->volume_key);
        cache->pkcs11_passwords = strv_free_erase(cache->pkcs11_passwords);
        cache->fido2_passwords = strv_free_erase(cache->fido2_passwords);
}

int password_cache_load_keyring(UserRecord *h, PasswordCache *cache) {
        _cleanup_free_ char *name = NULL;
        _cleanup_(erase_and_freep) void *vk = NULL;
        size_t vks;
        key_serial_t serial;
        int r;

        assert(h);
        assert(cache);

        name = strjoin("homework-user-", h->user_name);
        if (!name)
                return log_oom();

        serial = request_key("user", name, NULL, 0);
        if (serial == -1) {
                if (errno == ENOKEY)
                        return -ENOKEY;
                return log_error_errno(errno, "Failed to request key '%s': %m", name);
        }

        r = keyring_read(serial, &vk, &vks);
        if (r < 0)
                return log_error_errno(r, "Failed to read keyring key '%s': %m", name);

        log_info("Successfully acquired home volume key from kernel keyring.");

        erase_and_free(cache->volume_key);
        cache->volume_key = TAKE_PTR(vk);
        cache->volume_key_size = vks;

        return 0;
}
