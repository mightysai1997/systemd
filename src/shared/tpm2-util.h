/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>

#include "json.h"
#include "macro.h"
#include "sha256.h"

typedef enum TPM2Flags {
        TPM2_FLAGS_USE_PIN = 1 << 0,
} TPM2Flags;

#if HAVE_TPM2

#include <tss2/tss2_esys.h>
#include <tss2/tss2_mu.h>
#include <tss2/tss2_rc.h>

extern TSS2_RC (*sym_Esys_Create)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_PUBLIC *inPublic, const TPM2B_DATA *outsideInfo, const TPML_PCR_SELECTION *creationPCR, TPM2B_PRIVATE **outPrivate, TPM2B_PUBLIC **outPublic, TPM2B_CREATION_DATA **creationData, TPM2B_DIGEST **creationHash, TPMT_TK_CREATION **creationTicket);
extern TSS2_RC (*sym_Esys_CreateLoaded)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_TEMPLATE *inPublic, ESYS_TR *objectHandle, TPM2B_PRIVATE **outPrivate, TPM2B_PUBLIC **outPublic);
extern TSS2_RC (*sym_Esys_CreatePrimary)(ESYS_CONTEXT *esysContext, ESYS_TR primaryHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_PUBLIC *inPublic, const TPM2B_DATA *outsideInfo, const TPML_PCR_SELECTION *creationPCR, ESYS_TR *objectHandle, TPM2B_PUBLIC **outPublic, TPM2B_CREATION_DATA **creationData, TPM2B_DIGEST **creationHash, TPMT_TK_CREATION **creationTicket);
extern TSS2_RC (*sym_Esys_Duplicate)(ESYS_CONTEXT *esysContext, ESYS_TR objectHandle, ESYS_TR newParentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DATA *encryptionKeyIn, const TPMT_SYM_DEF_OBJECT *symmetricAlg, TPM2B_DATA **encryptionKeyOut, TPM2B_PRIVATE **duplicate, TPM2B_ENCRYPTED_SECRET **outSymSeed);
extern void (*sym_Esys_Finalize)(ESYS_CONTEXT **context);
extern TSS2_RC (*sym_Esys_FlushContext)(ESYS_CONTEXT *esysContext, ESYS_TR flushHandle);
extern void (*sym_Esys_Free)(void *ptr);
extern TSS2_RC (*sym_Esys_GetCapability)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2_CAP capability, UINT32 property, UINT32 propertyCount, TPMI_YES_NO *moreData, TPMS_CAPABILITY_DATA **capabilityData);
extern TSS2_RC (*sym_Esys_GetRandom)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, UINT16 bytesRequested, TPM2B_DIGEST **randomBytes);
extern TSS2_RC (*sym_Esys_Import)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DATA *encryptionKey, const TPM2B_PUBLIC *objectPublic, const TPM2B_PRIVATE *duplicate, const TPM2B_ENCRYPTED_SECRET *inSymSeed, const TPMT_SYM_DEF_OBJECT *symmetricAlg, TPM2B_PRIVATE **outPrivate);
extern TSS2_RC (*sym_Esys_Initialize)(ESYS_CONTEXT **esys_context,  TSS2_TCTI_CONTEXT *tcti, TSS2_ABI_VERSION *abiVersion);
extern TSS2_RC (*sym_Esys_Load)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_PRIVATE *inPrivate, const TPM2B_PUBLIC *inPublic, ESYS_TR *objectHandle);
extern TSS2_RC (*sym_Esys_LoadExternal)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE *inPrivate, const TPM2B_PUBLIC *inPublic, ESYS_TR hierarchy, ESYS_TR *objectHandle);
extern TSS2_RC (*sym_Esys_PCR_Extend)(ESYS_CONTEXT *esysContext, ESYS_TR pcrHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPML_DIGEST_VALUES *digests);
extern TSS2_RC (*sym_Esys_PCR_Read)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1,ESYS_TR shandle2, ESYS_TR shandle3, const TPML_PCR_SELECTION *pcrSelectionIn, UINT32 *pcrUpdateCounter, TPML_PCR_SELECTION **pcrSelectionOut, TPML_DIGEST **pcrValues);
extern TSS2_RC (*sym_Esys_PolicyAuthorize)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *approvedPolicy, const TPM2B_NONCE *policyRef, const TPM2B_NAME *keySign, const TPMT_TK_VERIFIED *checkTicket);
extern TSS2_RC (*sym_Esys_PolicyAuthValue)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3);
extern TSS2_RC (*sym_Esys_PolicyDuplicationSelect)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_NAME *objectName, const TPM2B_NAME *newParentName, TPMI_YES_NO includeObject);
extern TSS2_RC (*sym_Esys_PolicyGetDigest)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_DIGEST **policyDigest);
extern TSS2_RC (*sym_Esys_PolicyPCR)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *pcrDigest, const TPML_PCR_SELECTION *pcrs);
extern TSS2_RC (*sym_Esys_StartAuthSession)(ESYS_CONTEXT *esysContext, ESYS_TR tpmKey, ESYS_TR bind, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_NONCE *nonceCaller, TPM2_SE sessionType, const TPMT_SYM_DEF *symmetric, TPMI_ALG_HASH authHash, ESYS_TR *sessionHandle);
extern TSS2_RC (*sym_Esys_Startup)(ESYS_CONTEXT *esysContext, TPM2_SU startupType);
extern TSS2_RC (*sym_Esys_TRSess_GetAttributes)(ESYS_CONTEXT *esysContext, ESYS_TR session, TPMA_SESSION *flags);
extern TSS2_RC (*sym_Esys_TRSess_SetAttributes)(ESYS_CONTEXT *esysContext, ESYS_TR session, TPMA_SESSION flags, TPMA_SESSION mask);
extern TSS2_RC (*sym_Esys_TR_GetName)(ESYS_CONTEXT *esysContext, ESYS_TR handle, TPM2B_NAME **name);
extern TSS2_RC (*sym_Esys_TR_SetAuth)(ESYS_CONTEXT *esysContext, ESYS_TR handle, TPM2B_AUTH const *authValue);
extern TSS2_RC (*sym_Esys_Unseal)(ESYS_CONTEXT *esysContext, ESYS_TR itemHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_SENSITIVE_DATA **outData);
extern TSS2_RC (*sym_Esys_VerifySignature)(ESYS_CONTEXT *esysContext, ESYS_TR keyHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *digest, const TPMT_SIGNATURE *signature, TPMT_TK_VERIFIED **validation);

extern const char* (*sym_Tss2_RC_Decode)(TSS2_RC rc);

extern TSS2_RC (*sym_Tss2_MU_TPM2_CC_Marshal)(TPM2_CC src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPM2B_PRIVATE_Marshal)(TPM2B_PRIVATE const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPM2B_PRIVATE_Unmarshal)(uint8_t const buffer[], size_t buffer_size, size_t *offset, TPM2B_PRIVATE  *dest);
extern TSS2_RC (*sym_Tss2_MU_TPM2B_PUBLIC_Marshal)(TPM2B_PUBLIC const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPM2B_PUBLIC_Unmarshal)(uint8_t const buffer[], size_t buffer_size, size_t *offset, TPM2B_PUBLIC *dest);
extern TSS2_RC (*sym_Tss2_MU_TPML_PCR_SELECTION_Marshal)(TPML_PCR_SELECTION const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPMT_HA_Marshal)(TPMT_HA const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);
extern TSS2_RC (*sym_Tss2_MU_TPMT_PUBLIC_Marshal)(TPMT_PUBLIC const *src, uint8_t buffer[], size_t buffer_size, size_t *offset);

int dlopen_tpm2(void);

int tpm2_calculate_sealing_policy(const TPML_PCR_SELECTION *hash_pcr_selection, const TPM2B_DIGEST *hash_pcr_values, size_t hash_pcr_values_size, const void *pubkey, size_t pubkey_size, const char *pin, TPM2B_DIGEST *digest);
int tpm2_seal(const char *device, uint32_t hash_pcr_mask, const void *pubkey, size_t pubkey_size, uint32_t pubkey_pcr_mask, const char *pin, void **ret_secret, size_t *ret_secret_size, void **ret_blob, size_t *ret_blob_size, void **ret_pcr_hash, size_t *ret_pcr_hash_size, uint16_t *ret_pcr_bank, uint16_t *ret_primary_alg);
int tpm2_unseal(const char *device, uint32_t hash_pcr_mask, uint16_t pcr_bank, const void *pubkey, size_t pubkey_size, uint32_t pubkey_pcr_mask, JsonVariant *signature, const char *pin, uint16_t primary_alg, const void *blob, size_t blob_size, const void *policy_hash, size_t policy_hash_size, void **ret_secret, size_t *ret_secret_size);

struct tpm2_context {
        void *tcti_dl;
        TSS2_TCTI_CONTEXT *tcti_context;
        ESYS_CONTEXT *esys_context;
};

int tpm2_context_init(const char *device, struct tpm2_context *ret_context);
void tpm2_context_destroy(struct tpm2_context *c);

struct tpm2_handle {
        struct tpm2_context *context;
        ESYS_TR handle;
};

#define _tpm2_handle_init(c, h) (struct tpm2_handle) { .context = (c), .handle = (h), }
#define tpm2_handle_init(c) _tpm2_handle_init(c, ESYS_TR_NONE)
struct tpm2_handle tpm2_handle_release(struct tpm2_handle h);
static inline void tpm2_handle_releasep(struct tpm2_handle *h) {
        if (h)
                *h = tpm2_handle_release(*h);
}

#define HANDLE_NONE tpm2_handle_init(NULL)
#define HANDLE_RH_OWNER _tpm2_handle_init(NULL, ESYS_TR_RH_OWNER)
#define HANDLE_PASSWORD _tpm2_handle_init(NULL, ESYS_TR_PASSWORD)

/* Like TAKE_PTR() but for struct tpm2_handle, resetting them to HANDLE_NONE */
#define TAKE_HANDLE(handle) TAKE_GENERIC(handle, struct tpm2_handle, HANDLE_NONE)

static inline void Esys_Freep(void *p) {
        if (*(void**) p)
                sym_Esys_Free(*(void**) p);
}

int tpm2_get_good_pcr_banks(struct tpm2_context *c, uint32_t pcr_mask, TPMI_ALG_HASH **ret_banks);
int tpm2_get_good_pcr_banks_strv(struct tpm2_context *c, uint32_t pcr_mask, char ***ret);

int tpm2_extend_bytes(struct tpm2_context *c, char **banks, unsigned pcr_index, const void *data, size_t data_size, const void *secret, size_t secret_size);

TPML_PCR_SELECTION tpm2_tpml_pcr_selection_from_mask(uint32_t mask, TPMI_ALG_HASH hash);
uint32_t tpm2_tpml_pcr_selection_to_mask(const TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash);

int tpm2_list_devices(void);
int tpm2_find_device_auto(int log_level, char **ret);

#else /* HAVE_TPM2 */

struct tpm2_context {};
struct tpm2_handle {};

static inline int tpm2_not_supported(void) {
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "TPM2 not supported on this build.");
}

#define tpm2_context_init(...) tpm2_not_supported()
#define tpm2_context_destroy(...) tpm2_not_supported()
#define tpm2_handle_init(...) (struct tpm2_handle) {}
#define tpm2_handle_release(...) ({ tpm2_not_supported(); NULL; })
#define tpm2_handle_releasep(...) tpm2_not_supported()

#define tpm2_list_devices(...) tpm2_not_supported()
#define tpm2_find_device_auto(...) tpm2_not_supported()

#define tpm2_extend_bytes(...) tpm2_not_supported()

#endif /* HAVE_TPM2 */

int tpm2_parse_pcrs(const char *s, uint32_t *ret);

int tpm2_make_pcr_json_array(uint32_t pcr_mask, JsonVariant **ret);
int tpm2_parse_pcr_json_array(JsonVariant *v, uint32_t *ret);

int tpm2_make_luks2_json(int keyslot, uint32_t hash_pcr_mask, uint16_t pcr_bank, const void *pubkey, size_t pubkey_size, uint32_t pubkey_pcr_mask, uint16_t primary_alg, const void *blob, size_t blob_size, const void *policy_hash, size_t policy_hash_size, const void *salt, size_t salt_size, TPM2Flags flags, JsonVariant **ret);
int tpm2_parse_luks2_json(JsonVariant *v, int *ret_keyslot, uint32_t *ret_hash_pcr_mask, uint16_t *ret_pcr_bank, void **ret_pubkey, size_t *ret_pubkey_size, uint32_t *ret_pubkey_pcr_mask, uint16_t *ret_primary_alg, void **ret_blob, size_t *ret_blob_size, void **ret_policy_hash, size_t *ret_policy_hash_size, void **ret_salt, size_t *ret_salt_size, TPM2Flags *ret_flags);

#define TPM2_PCRS_MAX 24U

static inline bool TPM2_PCR_MASK_VALID(uint64_t pcr_mask) {
        return pcr_mask < (UINT64_C(1) << TPM2_PCRS_MAX); /* Support 24 PCR banks */
}

/* Default to PCR 7 only */
#define TPM2_PCR_MASK_DEFAULT (UINT32_C(1) << 7)

/* We want the helpers below to work also if TPM2 libs are not available, hence define these four defines if
 * they are missing. */
#ifndef TPM2_ALG_SHA1
#define TPM2_ALG_SHA1 0x4
#endif

#ifndef TPM2_ALG_SHA256
#define TPM2_ALG_SHA256 0xB
#endif

#ifndef TPM2_ALG_SHA384
#define TPM2_ALG_SHA384 0xC
#endif

#ifndef TPM2_ALG_SHA512
#define TPM2_ALG_SHA512 0xD
#endif

#ifndef TPM2_ALG_ECC
#define TPM2_ALG_ECC 0x23
#endif

#ifndef TPM2_ALG_RSA
#define TPM2_ALG_RSA 0x1
#endif

const char *tpm2_pcr_bank_to_string(uint16_t bank);
int tpm2_pcr_bank_from_string(const char *bank);

const char *tpm2_alg_to_string(uint16_t alg);
int tpm2_alg_from_string(const char *alg);

typedef struct {
        uint32_t search_pcr_mask;
        const char *device;
        const char *signature_path;
} systemd_tpm2_plugin_params;

typedef enum Tpm2Support {
        /* NOTE! The systemd-creds tool returns these flags 1:1 as exit status. Hence these flags are pretty
         * much ABI! Hence, be extra careful when changing/extending these definitions. */
        TPM2_SUPPORT_NONE      = 0,       /* no support */
        TPM2_SUPPORT_FIRMWARE  = 1 << 0,  /* firmware reports TPM2 was used */
        TPM2_SUPPORT_DRIVER    = 1 << 1,  /* the kernel has a driver loaded for it */
        TPM2_SUPPORT_SYSTEM    = 1 << 2,  /* we support it ourselves */
        TPM2_SUPPORT_SUBSYSTEM = 1 << 3,  /* the kernel has the tpm subsystem enabled */
        TPM2_SUPPORT_FULL      = TPM2_SUPPORT_FIRMWARE|TPM2_SUPPORT_DRIVER|TPM2_SUPPORT_SYSTEM|TPM2_SUPPORT_SUBSYSTEM,
} Tpm2Support;

Tpm2Support tpm2_support(void);

int tpm2_parse_pcr_argument(const char *arg, uint32_t *mask);

int tpm2_load_pcr_signature(const char *path, JsonVariant **ret);
int tpm2_load_pcr_public_key(const char *path, void **ret_pubkey, size_t *ret_pubkey_size);

int pcr_mask_to_string(uint32_t mask, char **ret);

int tpm2_util_pbkdf2_hmac_sha256(const void *pass,
                    size_t passlen,
                    const void *salt,
                    size_t saltlen,
                    uint8_t res[static SHA256_DIGEST_SIZE]);
