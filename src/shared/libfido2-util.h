/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "macro.h"

typedef enum Fido2EnrollFlags {
        FIDO2ENROLL_PIN           = 1 << 0,
        FIDO2ENROLL_UP            = 1 << 1, /* User presence (ie: touching token) */
        FIDO2ENROLL_UV            = 1 << 2, /* User verification (ie: fingerprint) */
        FIDO2ENROLL_PIN_IF_NEEDED = 1 << 3, /* If auth doesn't work without PIN ask for one, as in systemd 248 */
        FIDO2ENROLL_UP_IF_NEEDED  = 1 << 4, /* If auth doesn't work without UP, enable it, as in systemd 248 */
        FIDO2ENROLL_UV_OMIT       = 1 << 5, /* Leave "uv" untouched, as in systemd 248 */
        _FIDO2ENROLL_TYPE_MAX,
        _FIDO2ENROLL_TYPE_INVALID = -EINVAL,
} Fido2EnrollFlags;

#if HAVE_LIBFIDO2
#include <fido.h>

#include "dlfcn-util.h"

DLSYM_PROTOTYPE(fido_assert_allow_cred);
DLSYM_PROTOTYPE(fido_assert_free);
DLSYM_PROTOTYPE(fido_assert_hmac_secret_len);
DLSYM_PROTOTYPE(fido_assert_hmac_secret_ptr);
DLSYM_PROTOTYPE(fido_assert_new);
DLSYM_PROTOTYPE(fido_assert_set_clientdata_hash);
DLSYM_PROTOTYPE(fido_assert_set_extensions);
DLSYM_PROTOTYPE(fido_assert_set_hmac_salt);
DLSYM_PROTOTYPE(fido_assert_set_rp);
DLSYM_PROTOTYPE(fido_assert_set_up);
DLSYM_PROTOTYPE(fido_assert_set_uv);
DLSYM_PROTOTYPE(fido_cbor_info_extensions_len);
DLSYM_PROTOTYPE(fido_cbor_info_extensions_ptr);
DLSYM_PROTOTYPE(fido_cbor_info_free);
DLSYM_PROTOTYPE(fido_cbor_info_new);
DLSYM_PROTOTYPE(fido_cbor_info_options_len);
DLSYM_PROTOTYPE(fido_cbor_info_options_name_ptr);
DLSYM_PROTOTYPE(fido_cbor_info_options_value_ptr);
DLSYM_PROTOTYPE(fido_cred_free);
DLSYM_PROTOTYPE(fido_cred_id_len);
DLSYM_PROTOTYPE(fido_cred_id_ptr);
DLSYM_PROTOTYPE(fido_cred_new);
DLSYM_PROTOTYPE(fido_cred_set_clientdata_hash);
DLSYM_PROTOTYPE(fido_cred_set_extensions);
DLSYM_PROTOTYPE(fido_cred_set_prot);
DLSYM_PROTOTYPE(fido_cred_set_rk);
DLSYM_PROTOTYPE(fido_cred_set_rp);
DLSYM_PROTOTYPE(fido_cred_set_type);
DLSYM_PROTOTYPE(fido_cred_set_user);
DLSYM_PROTOTYPE(fido_cred_set_uv);
DLSYM_PROTOTYPE(fido_dev_free);
DLSYM_PROTOTYPE(fido_dev_get_assert);
DLSYM_PROTOTYPE(fido_dev_get_cbor_info);
DLSYM_PROTOTYPE(fido_dev_info_free);
DLSYM_PROTOTYPE(fido_dev_info_manifest);
DLSYM_PROTOTYPE(fido_dev_info_manufacturer_string);
DLSYM_PROTOTYPE(fido_dev_info_product_string);
DLSYM_PROTOTYPE(fido_dev_info_new);
DLSYM_PROTOTYPE(fido_dev_info_path);
DLSYM_PROTOTYPE(fido_dev_info_ptr);
DLSYM_PROTOTYPE(fido_dev_is_fido2);
DLSYM_PROTOTYPE(fido_dev_make_cred);
DLSYM_PROTOTYPE(fido_dev_new);
DLSYM_PROTOTYPE(fido_dev_open);
DLSYM_PROTOTYPE(fido_dev_close);
DLSYM_PROTOTYPE(fido_init);
DLSYM_PROTOTYPE(fido_set_log_handler);
DLSYM_PROTOTYPE(fido_strerr);

int dlopen_libfido2(void);

static inline void fido_cbor_info_free_wrapper(fido_cbor_info_t **p) {
        if (*p)
                sym_fido_cbor_info_free(p);
}

static inline void fido_assert_free_wrapper(fido_assert_t **p) {
        if (*p)
                sym_fido_assert_free(p);
}

static inline void fido_dev_free_wrapper(fido_dev_t **p) {
        if (*p) {
                sym_fido_dev_close(*p);
                sym_fido_dev_free(p);
        }
}

static inline void fido_cred_free_wrapper(fido_cred_t **p) {
        if (*p)
                sym_fido_cred_free(p);
}

int fido2_use_hmac_hash(
                const char *device,
                const char *rp_id,
                const void *salt,
                size_t salt_size,
                const void *cid,
                size_t cid_size,
                char **pins,
                Fido2EnrollFlags required,
                void **ret_hmac,
                size_t *ret_hmac_size);

int fido2_generate_hmac_hash(
                const char *device,
                const char *rp_id,
                const char *rp_name,
                const void *user_id, size_t user_id_len,
                const char *user_name,
                const char *user_display_name,
                const char *user_icon,
                const char *askpw_icon,
                const char *askpw_credential,
                Fido2EnrollFlags lock_with,
                int cred_alg,
                void **ret_cid, size_t *ret_cid_size,
                void **ret_salt, size_t *ret_salt_size,
                void **ret_secret, size_t *ret_secret_size,
                char **ret_usedpin,
                Fido2EnrollFlags *ret_locked_with);

int parse_fido2_algorithm(const char *s, int *ret);
#else
static inline int parse_fido2_algorithm(const char *s, int *ret) {
        return -EOPNOTSUPP;
}
#endif

int fido2_list_devices(void);
int fido2_find_device_auto(char **ret);

int fido2_have_device(const char *device);
