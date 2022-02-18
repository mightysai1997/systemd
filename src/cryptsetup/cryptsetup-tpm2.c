/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "ask-password-api.h"
#include "cryptsetup-tpm2.h"
#include "env-util.h"
#include "fileio.h"
#include "hexdecoct.h"
#include "json.h"
#include "parse-util.h"
#include "random-util.h"
#include "tpm2-util.h"

static int get_pin(usec_t until, AskPasswordFlags ask_password_flags, bool headless, char **ret_pin_str) {
        char *pin_str = NULL;
        int r = 0;
        const char *e;
        _cleanup_strv_free_erase_ char **pin = NULL;

        e = getenv("PIN");
        if (e) {
                pin_str = strdup(e);
                if (!pin_str)
                        return log_oom();

                assert_se(unsetenv_erase("PIN") >= 0);
        } else {
                size_t pin_len;

                if (headless)
                        return log_error_errno(
                                        SYNTHETIC_ERRNO(ENOPKG),
                                        "PIN querying disabled via 'headless' option. "
                                        "Use the '$PIN' environment variable.");

                pin = strv_free_erase(pin);
                r = ask_password_auto(
                        "Please enter TPM2 PIN:",
                        "drive-harddisk",
                        NULL,
                        "tpm2-pin",
                        "cryptsetup.tpm2-pin",
                        until,
                        ask_password_flags,
                        &pin);
                if (r < 0)
                        return log_error_errno(r, "Failed to ask for user pin: %m");
                assert(strv_length(pin) == 1);

                /* Enforce a PIN length of at least 4 characters to avoid unnecessary unseal
                 * attempts, which increase the dictionary attack counter and of up to 32 characters
                 * (the maximum supported for authValue). */
                pin_len = strlen(*pin);
                if (pin_len >= 4 && pin_len <= 32) {
                        pin_str = strdup(*pin);
                        goto finish;
                }

                return log_error_errno(
                        SYNTHETIC_ERRNO(EPERM), "Incorrect PIN length (must be 4-32 characters)!");
        }

finish:
        *ret_pin_str = pin_str;

        return r;
}

int acquire_tpm2_key(
                const char *volume_name,
                const char *device,
                uint32_t pcr_mask,
                uint16_t pcr_bank,
                uint16_t primary_alg,
                const char *key_file,
                size_t key_file_size,
                uint64_t key_file_offset,
                const void *key_data,
                size_t key_data_size,
                const void *policy_hash,
                size_t policy_hash_size,
                int flags,
                usec_t until,
                bool headless,
                AskPasswordFlags ask_password_flags,
                void **ret_decrypted_key,
                size_t *ret_decrypted_key_size) {

        _cleanup_free_ void *loaded_blob = NULL;
        _cleanup_free_ char *auto_device = NULL;
        size_t blob_size;
        const void *blob;
        int r;

        if (!device) {
                r = tpm2_find_device_auto(LOG_DEBUG, &auto_device);
                if (r == -ENODEV)
                        return -EAGAIN; /* Tell the caller to wait for a TPM2 device to show up */
                if (r < 0)
                        return r;

                device = auto_device;
        }

        if (key_data) {
                blob = key_data;
                blob_size = key_data_size;
        } else {
                _cleanup_free_ char *bindname = NULL;

                /* If we read the salt via AF_UNIX, make this client recognizable */
                if (asprintf(&bindname, "@%" PRIx64"/cryptsetup-tpm2/%s", random_u64(), volume_name) < 0)
                        return log_oom();

                r = read_full_file_full(
                                AT_FDCWD, key_file,
                                key_file_offset == 0 ? UINT64_MAX : key_file_offset,
                                key_file_size == 0 ? SIZE_MAX : key_file_size,
                                READ_FULL_FILE_CONNECT_SOCKET,
                                bindname,
                                (char**) &loaded_blob, &blob_size);
                if (r < 0)
                        return r;

                blob = loaded_blob;
        }

        if (flags & TPM2_FLAGS_USE_PIN) {
                for (int i = 5;; i--) {
                        _cleanup_(erase_and_freep) char *pin_str = NULL;

                        if (i <= 0)
                                return -EACCES;

                        r = get_pin(until, ask_password_flags, headless, &pin_str);
                        if (r == -EPERM)
                                continue;
                        if (r < 0)
                                return r;

                        r = tpm2_unseal(
                                        device,
                                        pcr_mask,
                                        pcr_bank,
                                        primary_alg,
                                        blob,
                                        blob_size,
                                        policy_hash,
                                        policy_hash_size,
                                        pin_str,
                                        ret_decrypted_key,
                                        ret_decrypted_key_size);
                        /* We get this error in case there is an authentication policy mismatch. This should
                         * not happen, but this avoids confusing behavior, just in case. */
                        if (IN_SET(r, -EPERM, -ENOLCK))
                                return r;
                        if (r < 0)
                                continue;

                        return r;
                }
        } else
                return tpm2_unseal(
                                device,
                                pcr_mask,
                                pcr_bank,
                                primary_alg,
                                blob,
                                blob_size,
                                policy_hash,
                                policy_hash_size,
                                NULL,
                                ret_decrypted_key,
                                ret_decrypted_key_size);
}

int find_tpm2_auto_data(
                struct crypt_device *cd,
                uint32_t search_pcr_mask,
                int start_token,
                uint32_t *ret_pcr_mask,
                uint16_t *ret_pcr_bank,
                uint16_t *ret_primary_alg,
                void **ret_blob,
                size_t *ret_blob_size,
                void **ret_policy_hash,
                size_t *ret_policy_hash_size,
                int *ret_keyslot,
                int *ret_token,
                int *ret_flags) {

        _cleanup_free_ void *blob = NULL, *policy_hash = NULL;
        size_t blob_size = 0, policy_hash_size = 0;
        int r, keyslot = -1, token = -1;
        int flags = 0;
        uint32_t pcr_mask = 0;
        uint16_t pcr_bank = UINT16_MAX; /* default: pick automatically */
        uint16_t primary_alg = TPM2_ALG_ECC; /* ECC was the only supported algorithm in systemd < 250, use that as implied default, for compatibility */

        assert(cd);

        for (token = start_token; token < sym_crypt_token_max(CRYPT_LUKS2); token++) {
                _cleanup_(json_variant_unrefp) JsonVariant *v = NULL;
                JsonVariant *w, *e;
                int ks;

                r = cryptsetup_get_token_as_json(cd, token, "systemd-tpm2", &v);
                if (IN_SET(r, -ENOENT, -EINVAL, -EMEDIUMTYPE))
                        continue;
                if (r < 0)
                        return log_error_errno(r, "Failed to read JSON token data off disk: %m");

                ks = cryptsetup_get_keyslot_from_token(v);
                if (ks < 0) {
                        /* Handle parsing errors of the keyslots field gracefully, since it's not 'owned' by
                         * us, but by the LUKS2 spec */
                        log_warning_errno(ks, "Failed to extract keyslot index from TPM2 JSON data token %i, skipping: %m", token);
                        continue;
                }

                w = json_variant_by_key(v, "tpm2-pcrs");
                if (!w || !json_variant_is_array(w))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "TPM2 token data lacks 'tpm2-pcrs' field.");

                assert(pcr_mask == 0);
                JSON_VARIANT_ARRAY_FOREACH(e, w) {
                        uint64_t u;

                        if (!json_variant_is_number(e))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "TPM2 PCR is not a number.");

                        u = json_variant_unsigned(e);
                        if (u >= TPM2_PCRS_MAX)
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "TPM2 PCR number out of range.");

                        pcr_mask |= UINT32_C(1) << u;
                }

                if (search_pcr_mask != UINT32_MAX &&
                    search_pcr_mask != pcr_mask) /* PCR mask doesn't match what is configured, ignore this entry */
                        continue;

                assert(keyslot < 0);
                keyslot = ks;

                assert(pcr_bank == UINT16_MAX);
                assert(primary_alg == TPM2_ALG_ECC);

                /* The bank field is optional, since it was added in systemd 250 only. Before the bank was
                 * hardcoded to SHA256. */
                w = json_variant_by_key(v, "tpm2-pcr-bank");
                if (w) {
                        /* The PCR bank field is optional */

                        if (!json_variant_is_string(w))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "TPM2 PCR bank is not a string.");

                        r = tpm2_pcr_bank_from_string(json_variant_string(w));
                        if (r < 0)
                                return log_error_errno(r, "TPM2 PCR bank invalid or not supported: %s", json_variant_string(w));

                        pcr_bank = r;
                }

                /* The primary key algorithm field is optional, since it was also added in systemd 250
                 * only. Before the algorithm was hardcoded to ECC. */
                w = json_variant_by_key(v, "tpm2-primary-alg");
                if (w) {
                        /* The primary key algorithm is optional */

                        if (!json_variant_is_string(w))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                                       "TPM2 primary key algorithm is not a string.");

                        r = tpm2_primary_alg_from_string(json_variant_string(w));
                        if (r < 0)
                                return log_error_errno(r, "TPM2 primary key algorithm invalid or not supported: %s", json_variant_string(w));

                        primary_alg = r;
                }

                assert(!blob);
                w = json_variant_by_key(v, "tpm2-blob");
                if (!w || !json_variant_is_string(w))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "TPM2 token data lacks 'tpm2-blob' field.");

                r = unbase64mem(json_variant_string(w), SIZE_MAX, &blob, &blob_size);
                if (r < 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Invalid base64 data in 'tpm2-blob' field.");

                assert(!policy_hash);
                w = json_variant_by_key(v, "tpm2-policy-hash");
                if (!w || !json_variant_is_string(w))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "TPM2 token data lacks 'tpm2-policy-hash' field.");

                r = unhexmem(json_variant_string(w), SIZE_MAX, &policy_hash, &policy_hash_size);
                if (r < 0)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Invalid base64 data in 'tpm2-policy-hash' field.");

                /* The flags field is optional and used to encode newly introduced features */
                w = json_variant_by_key(v, "tpm2-flags");
                if (w) {
                        if (!json_variant_is_array(w)) {
                                return log_error_errno(
                                        SYNTHETIC_ERRNO(EINVAL),
                                        "TPM2 token data flags field is not an array.");
                        }
                        JSON_VARIANT_ARRAY_FOREACH(e, w) {
                                const char *fs = NULL;
                                if (!json_variant_is_string(e))
                                        return log_error_errno(
                                                SYNTHETIC_ERRNO(EINVAL),
                                                "TPM2 token data flag is not a string.");
                                fs = json_variant_string(e);
                                assert(fs);
                                r = tpm2_flag_from_string(fs);
                                if (r > 0)
                                        flags |= r;
                        }
                }

                break;
        }

        if (!blob)
                return log_error_errno(SYNTHETIC_ERRNO(ENXIO),
                                       "No valid TPM2 token data found.");

        if (start_token <= 0)
                log_info("Automatically discovered security TPM2 token unlocks volume.");

        *ret_pcr_mask = pcr_mask;
        *ret_blob = TAKE_PTR(blob);
        *ret_blob_size = blob_size;
        *ret_policy_hash = TAKE_PTR(policy_hash);
        *ret_policy_hash_size = policy_hash_size;
        *ret_keyslot = keyslot;
        *ret_token = token;
        *ret_pcr_bank = pcr_bank;
        *ret_primary_alg = primary_alg;
        *ret_flags = flags;

        return 0;
}
