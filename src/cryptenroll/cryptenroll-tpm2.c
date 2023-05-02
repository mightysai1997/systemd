/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "ask-password-api.h"
#include "cryptenroll-tpm2.h"
#include "env-util.h"
#include "fileio.h"
#include "hexdecoct.h"
#include "json.h"
#include "memory-util.h"
#include "random-util.h"
#include "sha256.h"
#include "tpm2-util.h"

static int search_policy_hash(
                struct crypt_device *cd,
                const void *hash,
                size_t hash_size) {

        int r;

        assert(cd);
        assert(hash || hash_size == 0);

        if (hash_size == 0)
                return 0;

        for (int token = 0; token < sym_crypt_token_max(CRYPT_LUKS2); token ++) {
                _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
                _cleanup_free_ void *thash = NULL;
                size_t thash_size = 0;
                int keyslot;
                JsonVariant *w;

                r = cryptsetup_get_token_as_json(cd, token, "systemd-tpm2", &v);
                if (IN_SET(r, -ENOENT, -EINVAL, -EMEDIUMTYPE))
                        continue;
                if (r < 0)
                        return log_error_errno(r, "Failed to read JSON token data off disk: %m");

                keyslot = cryptsetup_get_keyslot_from_token(v);
                if (keyslot < 0) {
                        /* Handle parsing errors of the keyslots field gracefully, since it's not 'owned' by
                         * us, but by the LUKS2 spec */
                        log_warning_errno(keyslot, "Failed to determine keyslot of JSON token %i, skipping: %m", token);
                        continue;
                }

                w = json_variant_by_key(v, "tpm2-policy-hash");
                if (!w || !json_variant_is_string(w))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "TPM2 token data lacks 'tpm2-policy-hash' field.");

                r = unhexmem(json_variant_string(w), SIZE_MAX, &thash, &thash_size);
                if (r < 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Invalid base64 data in 'tpm2-policy-hash' field.");

                if (memcmp_nn(hash, hash_size, thash, thash_size) == 0)
                        return keyslot; /* Found entry with same hash. */
        }

        return -ENOENT; /* Not found */
}

static int get_pin(char **ret_pin_str, TPM2Flags *ret_flags) {
        _cleanup_free_ char *pin_str = NULL;
        int r;
        TPM2Flags flags = 0;

        assert(ret_pin_str);
        assert(ret_flags);

        r = getenv_steal_erase("NEWPIN", &pin_str);
        if (r < 0)
                return log_error_errno(r, "Failed to acquire PIN from environment: %m");
        if (r > 0)
                flags |= TPM2_FLAGS_USE_PIN | TPM2_FLAGS_APPEND_SALT;
        else {
                for (size_t i = 5;; i--) {
                        _cleanup_strv_free_erase_ char **pin = NULL, **pin2 = NULL;

                        if (i <= 0)
                                return log_error_errno(
                                                SYNTHETIC_ERRNO(ENOKEY), "Too many attempts, giving up.");

                        pin = strv_free_erase(pin);
                        r = ask_password_auto(
                                        "Please enter TPM2 PIN:",
                                        "drive-harddisk",
                                        NULL,
                                        "tpm2-pin",
                                        "cryptenroll.tpm2-pin",
                                        USEC_INFINITY,
                                        0,
                                        &pin);
                        if (r < 0)
                                return log_error_errno(r, "Failed to ask for user pin: %m");
                        assert(strv_length(pin) == 1);

                        r = ask_password_auto(
                                        "Please enter TPM2 PIN (repeat):",
                                        "drive-harddisk",
                                        NULL,
                                        "tpm2-pin",
                                        "cryptenroll.tpm2-pin",
                                        USEC_INFINITY,
                                        0,
                                        &pin2);
                        if (r < 0)
                                return log_error_errno(r, "Failed to ask for user pin: %m");
                        assert(strv_length(pin) == 1);

                        if (strv_equal(pin, pin2)) {
                                pin_str = strdup(*pin);
                                if (!pin_str)
                                        return log_oom();
                                flags |= TPM2_FLAGS_USE_PIN | TPM2_FLAGS_APPEND_SALT;
                                break;
                        }

                        log_error("PINs didn't match, please try again!");
                }
        }

        *ret_flags = flags;
        *ret_pin_str = TAKE_PTR(pin_str);

        return 0;
}

int enroll_tpm2(struct crypt_device *cd,
                const void *volume_key,
                size_t volume_key_size,
                const char *device,
                uint32_t hash_pcr_mask,
                const char *pubkey_path,
                uint32_t pubkey_pcr_mask,
                const char *signature_path,
                bool use_pin) {

        _cleanup_(erase_and_freep) void *secret = NULL;
        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL, *signature_json = NULL;
        _cleanup_(erase_and_freep) char *base64_encoded = NULL;
        _cleanup_(erase_and_freep) char *secret_and_salt = NULL;
        _cleanup_free_ void *srk_buf = NULL;
        size_t secret_size, blob_size, hash_size, pubkey_size = 0, srk_buf_size = 0;
        _cleanup_free_ void *blob = NULL, *hash = NULL, *pubkey = NULL;
        uint16_t pcr_bank, primary_alg;
        const char *node;
        _cleanup_(erase_and_freep) char *pin_str = NULL;
        ssize_t base64_encoded_size;
        size_t secret_and_salt_size;
        int r, keyslot;
        TPM2Flags flags = 0;
        uint8_t binary_salt[SHA256_DIGEST_SIZE] = {};
        uint8_t salted_pin[SHA256_DIGEST_SIZE] = {};
        /*
         * erase the salt, we'd rather attempt to not have this in a coredump
         * as an attacker would have all the parameters but pin used to create
         * the session key. This problem goes away when we move to a trusted
         * primary key, aka the SRK.
         */
        CLEANUP_ERASE(binary_salt);
        CLEANUP_ERASE(salted_pin);

        assert(cd);
        assert(volume_key);
        assert(volume_key_size > 0);
        assert(TPM2_PCR_MASK_VALID(hash_pcr_mask));
        assert(TPM2_PCR_MASK_VALID(pubkey_pcr_mask));

        assert_se(node = crypt_get_device_name(cd));

        if (use_pin) {
                r = get_pin(&pin_str, &flags);
                if (r < 0)
                        return r;

                r = crypto_random_bytes(binary_salt, sizeof(binary_salt));
                if (r < 0)
                        return log_error_errno(r, "Failed to acquire random salt: %m");

                r = tpm2_util_pbkdf2_hmac_sha256(pin_str, strlen(pin_str), binary_salt, sizeof(binary_salt), salted_pin);
                if (r < 0)
                        return log_error_errno(r, "Failed to perform PBKDF2: %m");

                pin_str = erase_and_free(pin_str);
                /* re-stringify pin_str */
                base64_encoded_size = base64mem(salted_pin, sizeof(salted_pin), &pin_str);
                if (base64_encoded_size < 0)
                        return log_error_errno(base64_encoded_size, "Failed to base64 encode salted pin: %m");
        }

        r = tpm2_load_pcr_public_key(pubkey_path, &pubkey, &pubkey_size);
        if (r < 0) {
                if (pubkey_path || signature_path || r != -ENOENT)
                        return log_error_errno(r, "Failed read TPM PCR public key: %m");

                log_debug_errno(r, "Failed to read TPM2 PCR public key, proceeding without: %m");
                pubkey_pcr_mask = 0;
        } else if (signature_path) {
                /* Also try to load the signature JSON object, to verify that our enrollment will work.
                 * This is optional however, skip it if it's not explicitly provided. */

                r = tpm2_load_pcr_signature(signature_path, &signature_json);
                if (r < 0)
                        return log_debug_errno(r, "Failed to read TPM PCR signature: %m");
        }

        r = tpm2_seal(device,
                      hash_pcr_mask,
                      pubkey, pubkey_size,
                      pubkey_pcr_mask,
                      pin_str,
                      &secret, &secret_size,
                      &blob, &blob_size,
                      &hash, &hash_size,
                      &pcr_bank,
                      &primary_alg,
                      &srk_buf,
                      &srk_buf_size);
        if (r < 0)
                return r;

        /* Let's see if we already have this specific PCR policy hash enrolled, if so, exit early. */
        r = search_policy_hash(cd, hash, hash_size);
        if (r == -ENOENT)
                log_debug_errno(r, "PCR policy hash not yet enrolled, enrolling now.");
        else if (r < 0)
                return r;
        else {
                log_info("This PCR set is already enrolled, executing no operation.");
                return r; /* return existing keyslot, so that wiping won't kill it */
        }

        /* Quick verification that everything is in order, we are not in a hurry after all. */
        if (!pubkey || signature_json) {
                _cleanup_(erase_and_freep) void *secret2 = NULL;
                size_t secret2_size;

                log_debug("Unsealing for verification...");
                r = tpm2_unseal(device,
                                hash_pcr_mask,
                                pcr_bank,
                                pubkey, pubkey_size,
                                pubkey_pcr_mask,
                                signature_json,
                                pin_str,
                                primary_alg,
                                blob, blob_size,
                                hash, hash_size,
                                srk_buf, srk_buf_size,
                                &secret2, &secret2_size);
                if (r < 0)
                        return r;

                if (memcmp_nn(secret, secret_size, secret2, secret2_size) != 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM2 seal/unseal verification failed.");
        }

        if (use_pin) {
                /* To prevent using a secret from a compromised TPM without the pin, append salted pin.
                 * See following attack on AMD's fTPMs: https://arxiv.org/abs/2304.14717 */
                secret_and_salt_size = secret_size + sizeof(salted_pin);
                secret_and_salt = malloc(secret_and_salt_size);
                if (!secret_and_salt)
                        return log_error_errno(-ENOMEM, "Failed to allocate volume decryption key buffer: %m");
                memcpy(secret_and_salt, secret, secret_size);
                memcpy(secret_and_salt + secret_size, salted_pin, siezof(salted_pin));
        }

        /* let's base64 encode the key to use, for compat with homed (and it's easier to every type it in by keyboard, if that might end up being necessary. */
        base64_encoded_size = base64mem(use_pin ? secret_and_salt : secret, use_pin ? secret_and_salt_size : secret_size, &base64_encoded);
        if (base64_encoded_size < 0)
                return log_error_errno(base64_encoded_size, "Failed to base64 encode secret key: %m");

        r = cryptsetup_set_minimal_pbkdf(cd);
        if (r < 0)
                return log_error_errno(r, "Failed to set minimal PBKDF: %m");

        keyslot = crypt_keyslot_add_by_volume_key(
                        cd,
                        CRYPT_ANY_SLOT,
                        volume_key,
                        volume_key_size,
                        base64_encoded,
                        base64_encoded_size);
        if (keyslot < 0)
                return log_error_errno(keyslot, "Failed to add new TPM2 key to %s: %m", node);

        r = tpm2_make_luks2_json(
                        keyslot,
                        hash_pcr_mask,
                        pcr_bank,
                        pubkey, pubkey_size,
                        pubkey_pcr_mask,
                        primary_alg,
                        blob, blob_size,
                        hash, hash_size,
                        use_pin ? binary_salt : NULL,
                        use_pin ? sizeof(binary_salt) : 0,
                        srk_buf, srk_buf_size,
                        flags,
                        &v);
        if (r < 0)
                return log_error_errno(r, "Failed to prepare TPM2 JSON token object: %m");

        r = cryptsetup_add_token_json(cd, v);
        if (r < 0)
                return log_error_errno(r, "Failed to add TPM2 JSON token to LUKS2 header: %m");

        log_info("New TPM2 token enrolled as key slot %i.", keyslot);
        return keyslot;
}
