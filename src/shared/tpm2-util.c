/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "constants.h"
#include "cryptsetup-util.h"
#include "dirent-util.h"
#include "dlfcn-util.h"
#include "efi-api.h"
#include "extract-word.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-table.h"
#include "fs-util.h"
#include "hexdecoct.h"
#include "hmac.h"
#include "initrd-util.h"
#include "lock-util.h"
#include "log.h"
#include "logarithm.h"
#include "memory-util.h"
#include "nulstr-util.h"
#include "parse-util.h"
#include "random-util.h"
#include "sha256.h"
#include "sort-util.h"
#include "stat-util.h"
#include "string-table.h"
#include "time-util.h"
#include "tpm2-util.h"
#include "virt.h"

#if HAVE_TPM2
static void *libtss2_esys_dl = NULL;
static void *libtss2_rc_dl = NULL;
static void *libtss2_mu_dl = NULL;

static TSS2_RC (*sym_Esys_Create)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_PUBLIC *inPublic, const TPM2B_DATA *outsideInfo, const TPML_PCR_SELECTION *creationPCR, TPM2B_PRIVATE **outPrivate, TPM2B_PUBLIC **outPublic, TPM2B_CREATION_DATA **creationData, TPM2B_DIGEST **creationHash, TPMT_TK_CREATION **creationTicket) = NULL;
static TSS2_RC (*sym_Esys_CreateLoaded)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_TEMPLATE *inPublic, ESYS_TR *objectHandle, TPM2B_PRIVATE **outPrivate, TPM2B_PUBLIC **outPublic) = NULL;
static TSS2_RC (*sym_Esys_CreatePrimary)(ESYS_CONTEXT *esysContext, ESYS_TR primaryHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE_CREATE *inSensitive, const TPM2B_PUBLIC *inPublic, const TPM2B_DATA *outsideInfo, const TPML_PCR_SELECTION *creationPCR, ESYS_TR *objectHandle, TPM2B_PUBLIC **outPublic, TPM2B_CREATION_DATA **creationData, TPM2B_DIGEST **creationHash, TPMT_TK_CREATION **creationTicket) = NULL;
static TSS2_RC (*sym_Esys_EvictControl)(ESYS_CONTEXT *esysContext, ESYS_TR auth, ESYS_TR objectHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPMI_DH_PERSISTENT persistentHandle, ESYS_TR *newObjectHandle) = NULL;
static void (*sym_Esys_Finalize)(ESYS_CONTEXT **context) = NULL;
static TSS2_RC (*sym_Esys_FlushContext)(ESYS_CONTEXT *esysContext, ESYS_TR flushHandle) = NULL;
static void (*sym_Esys_Free)(void *ptr) = NULL;
static TSS2_RC (*sym_Esys_GetCapability)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2_CAP capability, UINT32 property, UINT32 propertyCount, TPMI_YES_NO *moreData, TPMS_CAPABILITY_DATA **capabilityData) = NULL;
static TSS2_RC (*sym_Esys_GetRandom)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, UINT16 bytesRequested, TPM2B_DIGEST **randomBytes) = NULL;
static TSS2_RC (*sym_Esys_Import)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DATA *encryptionKey, const TPM2B_PUBLIC *objectPublic, const TPM2B_PRIVATE *duplicate, const TPM2B_ENCRYPTED_SECRET *inSymSeed, const TPMT_SYM_DEF_OBJECT *symmetricAlg, TPM2B_PRIVATE **outPrivate) = NULL;
static TSS2_RC (*sym_Esys_Initialize)(ESYS_CONTEXT **esys_context,  TSS2_TCTI_CONTEXT *tcti, TSS2_ABI_VERSION *abiVersion) = NULL;
static TSS2_RC (*sym_Esys_Load)(ESYS_CONTEXT *esysContext, ESYS_TR parentHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_PRIVATE *inPrivate, const TPM2B_PUBLIC *inPublic, ESYS_TR *objectHandle) = NULL;
static TSS2_RC (*sym_Esys_LoadExternal)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_SENSITIVE *inPrivate, const TPM2B_PUBLIC *inPublic, ESYS_TR hierarchy, ESYS_TR *objectHandle) = NULL;
static TSS2_RC (*sym_Esys_PCR_Extend)(ESYS_CONTEXT *esysContext, ESYS_TR pcrHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPML_DIGEST_VALUES *digests) = NULL;
static TSS2_RC (*sym_Esys_PCR_Read)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1,ESYS_TR shandle2, ESYS_TR shandle3, const TPML_PCR_SELECTION *pcrSelectionIn, UINT32 *pcrUpdateCounter, TPML_PCR_SELECTION **pcrSelectionOut, TPML_DIGEST **pcrValues) = NULL;
static TSS2_RC (*sym_Esys_PolicyAuthorize)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *approvedPolicy, const TPM2B_NONCE *policyRef, const TPM2B_NAME *keySign, const TPMT_TK_VERIFIED *checkTicket) = NULL;
static TSS2_RC (*sym_Esys_PolicyAuthValue)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3) = NULL;
static TSS2_RC (*sym_Esys_PolicyGetDigest)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_DIGEST **policyDigest) = NULL;
static TSS2_RC (*sym_Esys_PolicyPCR)(ESYS_CONTEXT *esysContext, ESYS_TR policySession, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *pcrDigest, const TPML_PCR_SELECTION *pcrs) = NULL;
static TSS2_RC (*sym_Esys_ReadPublic)(ESYS_CONTEXT *esysContext, ESYS_TR objectHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_PUBLIC **outPublic, TPM2B_NAME **name, TPM2B_NAME **qualifiedName) = NULL;
static TSS2_RC (*sym_Esys_StartAuthSession)(ESYS_CONTEXT *esysContext, ESYS_TR tpmKey, ESYS_TR bind, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_NONCE *nonceCaller, TPM2_SE sessionType, const TPMT_SYM_DEF *symmetric, TPMI_ALG_HASH authHash, ESYS_TR *sessionHandle) = NULL;
static TSS2_RC (*sym_Esys_Startup)(ESYS_CONTEXT *esysContext, TPM2_SU startupType) = NULL;
static TSS2_RC (*sym_Esys_TestParms)(ESYS_CONTEXT *esysContext, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPMT_PUBLIC_PARMS *parameters) = NULL;
static TSS2_RC (*sym_Esys_TR_Close)(ESYS_CONTEXT *esys_context, ESYS_TR *rsrc_handle) = NULL;
static TSS2_RC (*sym_Esys_TR_Deserialize)(ESYS_CONTEXT *esys_context, uint8_t const *buffer, size_t buffer_size, ESYS_TR *esys_handle) = NULL;
static TSS2_RC (*sym_Esys_TR_FromTPMPublic)(ESYS_CONTEXT *esysContext, TPM2_HANDLE tpm_handle, ESYS_TR optionalSession1, ESYS_TR optionalSession2, ESYS_TR optionalSession3, ESYS_TR *object) = NULL;
static TSS2_RC (*sym_Esys_TR_GetName)(ESYS_CONTEXT *esysContext, ESYS_TR handle, TPM2B_NAME **name) = NULL;
static TSS2_RC (*sym_Esys_TR_GetTpmHandle)(ESYS_CONTEXT *esys_context, ESYS_TR esys_handle, TPM2_HANDLE *tpm_handle) = NULL;
static TSS2_RC (*sym_Esys_TR_Serialize)(ESYS_CONTEXT *esys_context, ESYS_TR object, uint8_t **buffer, size_t *buffer_size) = NULL;
static TSS2_RC (*sym_Esys_TR_SetAuth)(ESYS_CONTEXT *esysContext, ESYS_TR handle, TPM2B_AUTH const *authValue) = NULL;
static TSS2_RC (*sym_Esys_TRSess_GetAttributes)(ESYS_CONTEXT *esysContext, ESYS_TR session, TPMA_SESSION *flags) = NULL;
static TSS2_RC (*sym_Esys_TRSess_SetAttributes)(ESYS_CONTEXT *esysContext, ESYS_TR session, TPMA_SESSION flags, TPMA_SESSION mask) = NULL;
static TSS2_RC (*sym_Esys_Unseal)(ESYS_CONTEXT *esysContext, ESYS_TR itemHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, TPM2B_SENSITIVE_DATA **outData) = NULL;
static TSS2_RC (*sym_Esys_VerifySignature)(ESYS_CONTEXT *esysContext, ESYS_TR keyHandle, ESYS_TR shandle1, ESYS_TR shandle2, ESYS_TR shandle3, const TPM2B_DIGEST *digest, const TPMT_SIGNATURE *signature, TPMT_TK_VERIFIED **validation) = NULL;

static TSS2_RC (*sym_Tss2_MU_TPM2_CC_Marshal)(TPM2_CC src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2_HANDLE_Marshal)(TPM2_HANDLE src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_DIGEST_Marshal)(TPM2B_DIGEST const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_ENCRYPTED_SECRET_Marshal)(TPM2B_ENCRYPTED_SECRET const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_ENCRYPTED_SECRET_Unmarshal)(uint8_t const buffer[], size_t buffer_size, size_t *offset, TPM2B_ENCRYPTED_SECRET *dest) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_NAME_Marshal)(TPM2B_NAME const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_PRIVATE_Marshal)(TPM2B_PRIVATE const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_PRIVATE_Unmarshal)(uint8_t const buffer[], size_t buffer_size, size_t *offset, TPM2B_PRIVATE  *dest) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_PUBLIC_Marshal)(TPM2B_PUBLIC const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_PUBLIC_Unmarshal)(uint8_t const buffer[], size_t buffer_size, size_t *offset, TPM2B_PUBLIC *dest) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPM2B_SENSITIVE_Marshal)(TPM2B_SENSITIVE const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPML_PCR_SELECTION_Marshal)(TPML_PCR_SELECTION const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPMS_ECC_POINT_Marshal)(TPMS_ECC_POINT const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPMT_HA_Marshal)(TPMT_HA const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_TPMT_PUBLIC_Marshal)(TPMT_PUBLIC const *src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;
static TSS2_RC (*sym_Tss2_MU_UINT32_Marshal)(UINT32 src, uint8_t buffer[], size_t buffer_size, size_t *offset) = NULL;

static const char* (*sym_Tss2_RC_Decode)(TSS2_RC rc) = NULL;

int dlopen_tpm2(void) {
        int r;

        r = dlopen_many_sym_or_warn(
                        &libtss2_esys_dl, "libtss2-esys.so.0", LOG_DEBUG,
                        DLSYM_ARG(Esys_Create),
                        DLSYM_ARG(Esys_CreateLoaded),
                        DLSYM_ARG(Esys_CreatePrimary),
                        DLSYM_ARG(Esys_EvictControl),
                        DLSYM_ARG(Esys_Finalize),
                        DLSYM_ARG(Esys_FlushContext),
                        DLSYM_ARG(Esys_Free),
                        DLSYM_ARG(Esys_GetCapability),
                        DLSYM_ARG(Esys_GetRandom),
                        DLSYM_ARG(Esys_Import),
                        DLSYM_ARG(Esys_Initialize),
                        DLSYM_ARG(Esys_Load),
                        DLSYM_ARG(Esys_LoadExternal),
                        DLSYM_ARG(Esys_PCR_Extend),
                        DLSYM_ARG(Esys_PCR_Read),
                        DLSYM_ARG(Esys_PolicyAuthorize),
                        DLSYM_ARG(Esys_PolicyAuthValue),
                        DLSYM_ARG(Esys_PolicyGetDigest),
                        DLSYM_ARG(Esys_PolicyPCR),
                        DLSYM_ARG(Esys_ReadPublic),
                        DLSYM_ARG(Esys_StartAuthSession),
                        DLSYM_ARG(Esys_Startup),
                        DLSYM_ARG(Esys_TestParms),
                        DLSYM_ARG(Esys_TR_Close),
                        DLSYM_ARG(Esys_TR_Deserialize),
                        DLSYM_ARG(Esys_TR_FromTPMPublic),
                        DLSYM_ARG(Esys_TR_GetName),
                        DLSYM_ARG(Esys_TR_Serialize),
                        DLSYM_ARG(Esys_TR_SetAuth),
                        DLSYM_ARG(Esys_TRSess_GetAttributes),
                        DLSYM_ARG(Esys_TRSess_SetAttributes),
                        DLSYM_ARG(Esys_Unseal),
                        DLSYM_ARG(Esys_VerifySignature));
        if (r < 0)
                return r;

        /* Esys_TR_GetTpmHandle was added to tpm2-tss in version 2.4.0. Once we can set a minimum tpm2-tss
         * version of 2.4.0 this sym can be moved up to the normal list above. */
        r = dlsym_many_or_warn(libtss2_esys_dl, LOG_DEBUG, DLSYM_ARG_FORCE(Esys_TR_GetTpmHandle));
        if (r < 0)
                log_debug("libtss2-esys too old, does not include Esys_TR_GetTpmHandle.");

        r = dlopen_many_sym_or_warn(
                        &libtss2_rc_dl, "libtss2-rc.so.0", LOG_DEBUG,
                        DLSYM_ARG(Tss2_RC_Decode));
        if (r < 0)
                return r;

        return dlopen_many_sym_or_warn(
                        &libtss2_mu_dl, "libtss2-mu.so.0", LOG_DEBUG,
                        DLSYM_ARG(Tss2_MU_TPM2_CC_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2_HANDLE_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_DIGEST_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_ENCRYPTED_SECRET_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_ENCRYPTED_SECRET_Unmarshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_NAME_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_PRIVATE_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_PRIVATE_Unmarshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_PUBLIC_Marshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_PUBLIC_Unmarshal),
                        DLSYM_ARG(Tss2_MU_TPM2B_SENSITIVE_Marshal),
                        DLSYM_ARG(Tss2_MU_TPML_PCR_SELECTION_Marshal),
                        DLSYM_ARG(Tss2_MU_TPMS_ECC_POINT_Marshal),
                        DLSYM_ARG(Tss2_MU_TPMT_HA_Marshal),
                        DLSYM_ARG(Tss2_MU_TPMT_PUBLIC_Marshal),
                        DLSYM_ARG(Tss2_MU_UINT32_Marshal));
}

static inline void Esys_Freep(void *p) {
        if (*(void**) p)
                sym_Esys_Free(*(void**) p);
}

/* Get a specific TPM capability (or capabilities).
 *
 * Returns 0 if there are no more capability properties of the requested type, or 1 if there are more, or < 0
 * on any error. Both 0 and 1 indicate this completed successfully, but do not indicate how many capability
 * properties were provided in 'ret_capability_data'. To find the number of provided properties, check the
 * specific type's 'count' field (e.g. for TPM2_CAP_ALGS, check ret_capability_data->algorithms.count).
 *
 * This calls TPM2_GetCapability() and does not alter the provided data, so it is important to understand how
 * that TPM function works. It is recommended to check the TCG TPM specification Part 3 ("Commands") section
 * on TPM2_GetCapability() for full details, but a short summary is: if this returns 0, all available
 * properties have been provided in ret_capability_data, or no properties were available. If this returns 1,
 * there are between 1 and "count" properties provided in ret_capability_data, and there are more available.
 * Note that this may provide less than "count" properties even if the TPM has more available. Also, each
 * capability category may have more specific requirements than described here; see the spec for exact
 * details. */
static int tpm2_get_capability(
                Tpm2Context *c,
                TPM2_CAP capability,
                uint32_t property,
                uint32_t count,
                TPMU_CAPABILITIES *ret_capability_data) {

        _cleanup_(Esys_Freep) TPMS_CAPABILITY_DATA *capabilities = NULL;
        TPMI_YES_NO more;
        TSS2_RC rc;

        assert(c);

        log_debug("Getting TPM2 capability 0x%04" PRIx32 " property 0x%04" PRIx32 " count %" PRIu32 ".",
                  capability, property, count);

        rc = sym_Esys_GetCapability(
                        c->esys_context,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        capability,
                        property,
                        count,
                        &more,
                        &capabilities);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to get TPM2 capability 0x%04" PRIx32 " property 0x%04" PRIx32 ": %s",
                                       capability, property, sym_Tss2_RC_Decode(rc));

        if (capabilities->capability != capability)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "TPM provided wrong capability: 0x%04" PRIx32 " instead of 0x%04" PRIx32 ".",
                                       capabilities->capability, capability);

        if (ret_capability_data)
                *ret_capability_data = capabilities->data;

        return more == TPM2_YES;
}

#define TPMA_CC_TO_TPM2_CC(cca) (((cca) & TPMA_CC_COMMANDINDEX_MASK) >> TPMA_CC_COMMANDINDEX_SHIFT)

static int tpm2_cache_capabilities(Tpm2Context *c) {
        TPMU_CAPABILITIES capability;
        int r;

        assert(c);

        /* Cache the algorithms. The spec indicates supported algorithms can only be modified during runtime
         * by the SetAlgorithmSet() command. Unfortunately, the spec doesn't require a TPM reinitialization
         * after changing the algorithm set (unless the PCR algorithms are changed). However, the spec also
         * indicates the TPM behavior after SetAlgorithmSet() is "vendor-dependent", giving the example of
         * flushing sessions and objects, erasing policies, etc. So, if the algorithm set is programmatically
         * changed while we are performing some operation, it's reasonable to assume it will break us even if
         * we don't cache the algorithms, thus they should be "safe" to cache. */
        TPM2_ALG_ID current_alg = TPM2_ALG_FIRST;
        for (;;) {
                r = tpm2_get_capability(
                                c,
                                TPM2_CAP_ALGS,
                                (uint32_t) current_alg, /* The spec states to cast TPM2_ALG_ID to uint32_t. */
                                TPM2_MAX_CAP_ALGS,
                                &capability);
                if (r < 0)
                        return r;

                TPML_ALG_PROPERTY algorithms = capability.algorithms;

                /* We should never get 0; the TPM must support some algorithms, and it must not set 'more' if
                 * there are no more. */
                assert(algorithms.count > 0);

                if (!GREEDY_REALLOC_APPEND(
                                c->capability_algorithms,
                                c->n_capability_algorithms,
                                algorithms.algProperties,
                                algorithms.count))
                        return log_oom();

                if (r == 0)
                        break;

                /* Set current_alg to alg id after last alg id the TPM provided */
                current_alg = algorithms.algProperties[algorithms.count - 1].alg + 1;
        }

        /* Cache the command capabilities. The spec isn't actually clear if commands can be added/removed
         * while running, but that would be crazy, so let's hope it is not possible. */
        TPM2_CC current_cc = TPM2_CC_FIRST;
        for (;;) {
                r = tpm2_get_capability(
                                c,
                                TPM2_CAP_COMMANDS,
                                current_cc,
                                TPM2_MAX_CAP_CC,
                                &capability);
                if (r < 0)
                        return r;

                TPML_CCA commands = capability.command;

                /* We should never get 0; the TPM must support some commands, and it must not set 'more' if
                 * there are no more. */
                assert(commands.count > 0);

                if (!GREEDY_REALLOC_APPEND(
                                c->capability_commands,
                                c->n_capability_commands,
                                commands.commandAttributes,
                                commands.count))
                        return log_oom();

                if (r == 0)
                        break;

                /* Set current_cc to index after last cc the TPM provided */
                current_cc = TPMA_CC_TO_TPM2_CC(commands.commandAttributes[commands.count - 1]) + 1;
        }

        /* Cache the PCR capabilities, which are safe to cache, as the only way they can change is
         * TPM2_PCR_Allocate(), which changes the allocation after the next _TPM_Init(). If the TPM is
         * reinitialized while we are using it, all our context and sessions will be invalid, so we can
         * safely assume the TPM PCR allocation will not change while we are using it. */
        r = tpm2_get_capability(
                        c,
                        TPM2_CAP_PCRS,
                        /* property= */ 0,
                        /* count= */ 1,
                        &capability);
        if (r < 0)
                return r;
        if (r == 1)
                /* This should never happen. Part 3 ("Commands") of the TCG TPM2 spec in the section for
                 * TPM2_GetCapability states: "TPM_CAP_PCRS – Returns the current allocation of PCR in a
                 * TPML_PCR_SELECTION. The property parameter shall be zero. The TPM will always respond to
                 * this command with the full PCR allocation and moreData will be NO." */
                log_warning("TPM bug: reported multiple PCR sets; using only first set.");
        c->capability_pcrs = capability.assignedPCR;

        return 0;
}

/* Get the TPMA_ALGORITHM for a TPM2_ALG_ID. Returns true if the TPM supports the algorithm and the
 * TPMA_ALGORITHM is provided, otherwise false. */
static bool tpm2_get_capability_alg(Tpm2Context *c, TPM2_ALG_ID alg, TPMA_ALGORITHM *ret) {
        assert(c);

        FOREACH_ARRAY(alg_prop, c->capability_algorithms, c->n_capability_algorithms)
                if (alg_prop->alg == alg) {
                        if (ret)
                                *ret = alg_prop->algProperties;
                        return true;
                }

        log_debug("TPM does not support alg 0x%02" PRIx16 ".", alg);
        if (ret)
                *ret = 0;

        return false;
}

bool tpm2_supports_alg(Tpm2Context *c, TPM2_ALG_ID alg) {
        return tpm2_get_capability_alg(c, alg, NULL);
}

/* Get the TPMA_CC for a TPM2_CC. Returns true if the TPM supports the command and the TPMA_CC is provided,
 * otherwise false. */
static bool tpm2_get_capability_command(Tpm2Context *c, TPM2_CC command, TPMA_CC *ret) {
        assert(c);

        FOREACH_ARRAY(cca, c->capability_commands, c->n_capability_commands)
                if (TPMA_CC_TO_TPM2_CC(*cca) == command) {
                        if (ret)
                                *ret = *cca;
                        return true;
                }

        log_debug("TPM does not support command 0x%04" PRIx32 ".", command);
        if (ret)
                *ret = 0;

        return false;
}

bool tpm2_supports_command(Tpm2Context *c, TPM2_CC command) {
        return tpm2_get_capability_command(c, command, NULL);
}

/* Returns 1 if the TPM supports the ECC curve, 0 if not, or < 0 for any error. */
static int tpm2_supports_ecc_curve(Tpm2Context *c, TPM2_ECC_CURVE curve) {
        TPMU_CAPABILITIES capability;
        int r;

        /* The spec explicitly states the TPM2_ECC_CURVE should be cast to uint32_t. */
        r = tpm2_get_capability(c, TPM2_CAP_ECC_CURVES, (uint32_t) curve, 1, &capability);
        if (r < 0)
                return r;

        TPML_ECC_CURVE eccCurves = capability.eccCurves;
        if (eccCurves.count == 0 || eccCurves.eccCurves[0] != curve) {
                log_debug("TPM does not support ECC curve 0x%02" PRIx16 ".", curve);
                return 0;
        }

        return 1;
}

/* Query the TPM for populated handles.
 *
 * This provides an array of handle indexes populated in the TPM, starting at the requested handle. The array will
 * contain only populated handle addresses (which might not include the requested handle). The number of
 * handles will be no more than the 'max' number requested. This will not search past the end of the handle
 * range (i.e. handle & 0xff000000).
 *
 * Returns 0 if all populated handles in the range (starting at the requested handle) were provided (or no
 * handles were in the range), or 1 if there are more populated handles in the range, or < 0 on any error. */
static int tpm2_get_capability_handles(
                Tpm2Context *c,
                TPM2_HANDLE start,
                size_t max,
                TPM2_HANDLE **ret_handles,
                size_t *ret_n_handles) {

        _cleanup_free_ TPM2_HANDLE *handles = NULL;
        size_t n_handles = 0;
        TPM2_HANDLE current = start;
        int r = 0;

        assert(c);
        assert(ret_handles);
        assert(ret_n_handles);

        while (max > 0) {
                TPMU_CAPABILITIES capability;
                r = tpm2_get_capability(c, TPM2_CAP_HANDLES, current, (uint32_t) max, &capability);
                if (r < 0)
                        return r;

                TPML_HANDLE handle_list = capability.handles;
                if (handle_list.count == 0)
                        break;

                assert(handle_list.count <= max);

                if (n_handles > SIZE_MAX - handle_list.count)
                        return log_oom();

                if (!GREEDY_REALLOC(handles, n_handles + handle_list.count))
                        return log_oom();

                memcpy_safe(&handles[n_handles], handle_list.handle, sizeof(handles[0]) * handle_list.count);

                max -= handle_list.count;
                n_handles += handle_list.count;

                /* Update current to the handle index after the last handle in the list. */
                current = handles[n_handles - 1] + 1;

                if (r == 0)
                        /* No more handles in this range. */
                        break;
        }

        *ret_handles = TAKE_PTR(handles);
        *ret_n_handles = n_handles;

        return r;
}

#define TPM2_HANDLE_RANGE(h) ((TPM2_HANDLE)((h) & TPM2_HR_RANGE_MASK))
#define TPM2_HANDLE_TYPE(h) ((TPM2_HT)(TPM2_HANDLE_RANGE(h) >> TPM2_HR_SHIFT))

/* Returns 1 if the handle is populated in the TPM, 0 if not, and < 0 on any error. */
static int tpm2_get_capability_handle(Tpm2Context *c, TPM2_HANDLE handle) {
        _cleanup_free_ TPM2_HANDLE *handles = NULL;
        size_t n_handles = 0;
        int r;

        r = tpm2_get_capability_handles(c, handle, 1, &handles, &n_handles);
        if (r < 0)
                return r;

        return n_handles == 0 ? false : handles[0] == handle;
}

/* Returns 1 if the TPM supports the parms, or 0 if the TPM does not support the parms. */
bool tpm2_test_parms(Tpm2Context *c, TPMI_ALG_PUBLIC alg, const TPMU_PUBLIC_PARMS *parms) {
        TSS2_RC rc;

        assert(c);
        assert(parms);

        TPMT_PUBLIC_PARMS parameters = {
                .type = alg,
                .parameters = *parms,
        };

        rc = sym_Esys_TestParms(c->esys_context, ESYS_TR_NONE, ESYS_TR_NONE, ESYS_TR_NONE, &parameters);
        if (rc != TSS2_RC_SUCCESS)
                /* The spec says if the parms are not supported the TPM returns "...the appropriate
                 * unmarshaling error if a parameter is not valid". Since the spec (currently) defines 15
                 * unmarshaling errors, instead of checking for them all here, let's just assume any error
                 * indicates unsupported parms, and log the specific error text. */
                log_debug("TPM does not support tested parms: %s", sym_Tss2_RC_Decode(rc));

        return rc == TSS2_RC_SUCCESS;
}

static inline bool tpm2_supports_tpmt_public(Tpm2Context *c, const TPMT_PUBLIC *public) {
        assert(c);
        assert(public);

        return tpm2_test_parms(c, public->type, &public->parameters);
}

static inline bool tpm2_supports_tpmt_sym_def_object(Tpm2Context *c, const TPMT_SYM_DEF_OBJECT *parameters) {
        assert(c);
        assert(parameters);

        TPMU_PUBLIC_PARMS parms = {
                .symDetail.sym = *parameters,
        };

        return tpm2_test_parms(c, TPM2_ALG_SYMCIPHER, &parms);
}

static inline bool tpm2_supports_tpmt_sym_def(Tpm2Context *c, const TPMT_SYM_DEF *parameters) {
        assert(c);
        assert(parameters);

        /* Unfortunately, TPMT_SYM_DEF and TPMT_SYM_DEF_OBEJECT are separately defined, even though they are
         * functionally identical. */
        TPMT_SYM_DEF_OBJECT object = {
                .algorithm = parameters->algorithm,
                .keyBits = parameters->keyBits,
                .mode = parameters->mode,
        };

        return tpm2_supports_tpmt_sym_def_object(c, &object);
}

static Tpm2Context *tpm2_context_free(Tpm2Context *c) {
        if (!c)
                return NULL;

        if (c->esys_context)
                sym_Esys_Finalize(&c->esys_context);

        c->tcti_context = mfree(c->tcti_context);
        c->tcti_dl = safe_dlclose(c->tcti_dl);

        c->capability_algorithms = mfree(c->capability_algorithms);
        c->capability_commands = mfree(c->capability_commands);

        return mfree(c);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(Tpm2Context, tpm2_context, tpm2_context_free);

static const TPMT_SYM_DEF SESSION_TEMPLATE_SYM_AES_128_CFB = {
        .algorithm = TPM2_ALG_AES,
        .keyBits.aes = 128,
        .mode.aes = TPM2_ALG_CFB, /* The spec requires sessions to use CFB. */
};

int tpm2_context_new(const char *device, Tpm2Context **ret_context) {
        _cleanup_(tpm2_context_unrefp) Tpm2Context *context = NULL;
        TSS2_RC rc;
        int r;

        assert(ret_context);

        context = new(Tpm2Context, 1);
        if (!context)
                return log_oom();

        *context = (Tpm2Context) {
                .n_ref = 1,
        };

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        if (!device) {
                device = secure_getenv("SYSTEMD_TPM2_DEVICE");
                if (device)
                        /* Setting the env var to an empty string forces tpm2-tss' own device picking
                         * logic to be used. */
                        device = empty_to_null(device);
                else
                        /* If nothing was specified explicitly, we'll use a hardcoded default: the "device" tcti
                         * driver and the "/dev/tpmrm0" device. We do this since on some distributions the tpm2-abrmd
                         * might be used and we really don't want that, since it is a system service and that creates
                         * various ordering issues/deadlocks during early boot. */
                        device = "device:/dev/tpmrm0";
        }

        if (device) {
                const char *param, *driver, *fn;
                const TSS2_TCTI_INFO* info;
                TSS2_TCTI_INFO_FUNC func;
                size_t sz = 0;

                param = strchr(device, ':');
                if (param) {
                        /* Syntax #1: Pair of driver string and arbitrary parameter */
                        driver = strndupa_safe(device, param - device);
                        if (isempty(driver))
                                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 driver name is empty, refusing.");

                        param++;
                } else if (path_is_absolute(device) && path_is_valid(device)) {
                        /* Syntax #2: TPM device node */
                        driver = "device";
                        param = device;
                } else
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid TPM2 driver string, refusing.");

                log_debug("Using TPM2 TCTI driver '%s' with device '%s'.", driver, param);

                fn = strjoina("libtss2-tcti-", driver, ".so.0");

                /* Better safe than sorry, let's refuse strings that cannot possibly be valid driver early, before going to disk. */
                if (!filename_is_valid(fn))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 driver name '%s' not valid, refusing.", driver);

                context->tcti_dl = dlopen(fn, RTLD_NOW);
                if (!context->tcti_dl)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Failed to load %s: %s", fn, dlerror());

                func = dlsym(context->tcti_dl, TSS2_TCTI_INFO_SYMBOL);
                if (!func)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to find TCTI info symbol " TSS2_TCTI_INFO_SYMBOL ": %s",
                                               dlerror());

                info = func();
                if (!info)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Unable to get TCTI info data.");

                log_debug("Loaded TCTI module '%s' (%s) [Version %" PRIu32 "]", info->name, info->description, info->version);

                rc = info->init(NULL, &sz, NULL);
                if (rc != TPM2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to initialize TCTI context: %s", sym_Tss2_RC_Decode(rc));

                context->tcti_context = malloc0(sz);
                if (!context->tcti_context)
                        return log_oom();

                rc = info->init(context->tcti_context, &sz, param);
                if (rc != TPM2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to initialize TCTI context: %s", sym_Tss2_RC_Decode(rc));
        }

        rc = sym_Esys_Initialize(&context->esys_context, context->tcti_context, NULL);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to initialize TPM context: %s", sym_Tss2_RC_Decode(rc));

        rc = sym_Esys_Startup(context->esys_context, TPM2_SU_CLEAR);
        if (rc == TPM2_RC_INITIALIZE)
                log_debug("TPM already started up.");
        else if (rc == TSS2_RC_SUCCESS)
                log_debug("TPM successfully started up.");
        else
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to start up TPM: %s", sym_Tss2_RC_Decode(rc));

        r = tpm2_cache_capabilities(context);
        if (r < 0)
                return r;

        /* We require AES and CFB support for session encryption. */
        if (!tpm2_supports_alg(context, TPM2_ALG_AES))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM does not support AES.");

        if (!tpm2_supports_alg(context, TPM2_ALG_CFB))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM does not support CFB.");

        if (!tpm2_supports_tpmt_sym_def(context, &SESSION_TEMPLATE_SYM_AES_128_CFB))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM does not support AES-128-CFB.");

        *ret_context = TAKE_PTR(context);

        return 0;
}

static void tpm2_handle_cleanup(ESYS_CONTEXT *esys_context, ESYS_TR esys_handle, bool flush) {
        TSS2_RC rc;

        if (!esys_context || esys_handle == ESYS_TR_NONE)
                return;

        /* Closing the handle removes its reference from the esys_context, but leaves the corresponding
         * handle in the actual TPM. Flushing the handle removes its reference from the esys_context as well
         * as removing its corresponding handle from the actual TPM. */
        if (flush)
                rc = sym_Esys_FlushContext(esys_context, esys_handle);
        else
                rc = sym_Esys_TR_Close(esys_context, &esys_handle);
        if (rc != TSS2_RC_SUCCESS) /* We ignore failures here (besides debug logging), since this is called
                                    * in error paths, where we cannot do anything about failures anymore. And
                                    * when it is called in successful codepaths by this time we already did
                                    * what we wanted to do, and got the results we wanted so there's no
                                    * reason to make this fail more loudly than necessary. */
                log_debug("Failed to %s TPM handle, ignoring: %s", flush ? "flush" : "close", sym_Tss2_RC_Decode(rc));
}

Tpm2Handle *tpm2_handle_free(Tpm2Handle *handle) {
        if (!handle)
                return NULL;

        _cleanup_(tpm2_context_unrefp) Tpm2Context *context = (Tpm2Context*)handle->tpm2_context;
        if (context)
                tpm2_handle_cleanup(context->esys_context, handle->esys_handle, handle->flush);

        return mfree(handle);
}

int tpm2_handle_new(Tpm2Context *context, Tpm2Handle **ret_handle) {
        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;

        assert(ret_handle);

        handle = new(Tpm2Handle, 1);
        if (!handle)
                return log_oom();

        *handle = (Tpm2Handle) {
                .tpm2_context = tpm2_context_ref(context),
                .esys_handle = ESYS_TR_NONE,
                .flush = true,
        };

        *ret_handle = TAKE_PTR(handle);

        return 0;
}

/* Create a Tpm2Handle object that references a pre-existing handle in the TPM, at the TPM2_HANDLE address
 * provided. This should be used only for persistent, transient, or NV handles. Returns 1 on success, 0 if
 * the requested handle is not present in the TPM, or < 0 on error. */
static int tpm2_esys_handle_from_tpm_handle(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2_HANDLE tpm_handle,
                Tpm2Handle **ret_handle) {

        TSS2_RC rc;
        int r;

        assert(c);
        assert(tpm_handle > 0);
        assert(ret_handle);

        /* Let's restrict this, at least for now, to allow only some handle types. */
        switch (TPM2_HANDLE_TYPE(tpm_handle)) {
        case TPM2_HT_PERSISTENT:
        case TPM2_HT_NV_INDEX:
        case TPM2_HT_TRANSIENT:
                break;
        case TPM2_HT_PCR:
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Refusing to create ESYS handle for PCR handle 0x%08" PRIx32 ".",
                                       tpm_handle);
        case TPM2_HT_HMAC_SESSION:
        case TPM2_HT_POLICY_SESSION:
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Refusing to create ESYS handle for session handle 0x%08" PRIx32 ".",
                                       tpm_handle);
        case TPM2_HT_PERMANENT: /* Permanent handles are defined, e.g. ESYS_TR_RH_OWNER. */
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Refusing to create ESYS handle for permanent handle 0x%08" PRIx32 ".",
                                       tpm_handle);
        default:
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Refusing to create ESYS handle for unknown handle 0x%08" PRIx32 ".",
                                       tpm_handle);
        }

        r = tpm2_get_capability_handle(c, tpm_handle);
        if (r < 0)
                return r;
        if (r == 0) {
                log_debug("TPM handle 0x%08" PRIx32 " not populated.", tpm_handle);
                *ret_handle = NULL;
                return 0;
        }

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        /* Since we didn't create this handle in the TPM (this is only creating an ESYS_TR handle for the
         * pre-existing TPM handle), we shouldn't flush (or evict) it on cleanup. */
        handle->flush = false;

        rc = sym_Esys_TR_FromTPMPublic(
                        c->esys_context,
                        tpm_handle,
                        session ? session->esys_handle : ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &handle->esys_handle);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to read public info: %s", sym_Tss2_RC_Decode(rc));

        *ret_handle = TAKE_PTR(handle);

        return 1;
}

/* Copy an object in the TPM at a transient location to a persistent location.
 *
 * The provided transient handle must exist in the TPM in the transient range. The persistent location may be
 * 0 or any location in the persistent range. If 0, this will try each handle in the persistent range, in
 * ascending order, until an available one is found. If non-zero, only the requested persistent location will
 * be used.
 *
 * Returns 1 if the object was successfully persisted, or 0 if there is already a key at the requested
 * location(s), or < 0 on error. The persistent handle is only provided when returning 1. */
static int tpm2_persist_handle(
                Tpm2Context *c,
                const Tpm2Handle *transient_handle,
                const Tpm2Handle *session,
                TPMI_DH_PERSISTENT persistent_location,
                Tpm2Handle **ret_persistent_handle) {

        /* We don't use TPM2_PERSISTENT_FIRST and TPM2_PERSISTENT_LAST here due to:
         * https://github.com/systemd/systemd/pull/27713#issuecomment-1591864753 */
        TPMI_DH_PERSISTENT first = UINT32_C(0x81000000), last = UINT32_C(0x81ffffff);
        TSS2_RC rc;
        int r;

        assert(c);
        assert(transient_handle);

        /* If persistent location specified, only try that. */
        if (persistent_location != 0) {
                if (TPM2_HANDLE_TYPE(persistent_location) != TPM2_HT_PERSISTENT)
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "Handle not in persistent range: 0x%x", persistent_location);

                first = last = persistent_location;
        }

        for (TPMI_DH_PERSISTENT requested = first; requested <= last; requested++) {
                _cleanup_(tpm2_handle_freep) Tpm2Handle *persistent_handle = NULL;
                r = tpm2_handle_new(c, &persistent_handle);
                if (r < 0)
                        return r;

                /* Since this is a persistent handle, don't flush it. */
                persistent_handle->flush = false;

                rc = sym_Esys_EvictControl(
                                c->esys_context,
                                ESYS_TR_RH_OWNER,
                                transient_handle->esys_handle,
                                session ? session->esys_handle : ESYS_TR_PASSWORD,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                requested,
                                &persistent_handle->esys_handle);
                if (rc == TSS2_RC_SUCCESS) {
                        if (ret_persistent_handle)
                                *ret_persistent_handle = TAKE_PTR(persistent_handle);

                        return 1;
                }
                if (rc != TPM2_RC_NV_DEFINED)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to persist handle: %s", sym_Tss2_RC_Decode(rc));
        }

        if (ret_persistent_handle)
                *ret_persistent_handle = NULL;

        return 0;
}

#define TPM2_CREDIT_RANDOM_FLAG_PATH "/run/systemd/tpm-rng-credited"

static int tpm2_credit_random(Tpm2Context *c) {
        size_t rps, done = 0;
        TSS2_RC rc;
        usec_t t;
        int r;

        assert(c);

        /* Pulls some entropy from the TPM and adds it into the kernel RNG pool. That way we can say that the
         * key we will ultimately generate with the kernel random pool is at least as good as the TPM's RNG,
         * but likely better. Note that we don't trust the TPM RNG very much, hence do not actually credit
         * any entropy. */

        if (access(TPM2_CREDIT_RANDOM_FLAG_PATH, F_OK) < 0) {
                if (errno != ENOENT)
                        log_debug_errno(errno, "Failed to detect if '" TPM2_CREDIT_RANDOM_FLAG_PATH "' exists, ignoring: %m");
        } else {
                log_debug("Not adding TPM2 entropy to the kernel random pool again.");
                return 0; /* Already done */
        }

        t = now(CLOCK_MONOTONIC);

        for (rps = random_pool_size(); rps > 0;) {
                _cleanup_(Esys_Freep) TPM2B_DIGEST *buffer = NULL;

                rc = sym_Esys_GetRandom(
                                c->esys_context,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                MIN(rps, 32U), /* 32 is supposedly a safe choice, given that AES 256bit keys are this long, and TPM2 baseline requires support for those. */
                                &buffer);
                if (rc != TSS2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to acquire entropy from TPM: %s", sym_Tss2_RC_Decode(rc));

                if (buffer->size == 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Zero-sized entropy returned from TPM.");

                r = random_write_entropy(-1, buffer->buffer, buffer->size, /* credit= */ false);
                if (r < 0)
                        return log_error_errno(r, "Failed wo write entropy to kernel: %m");

                done += buffer->size;
                rps = LESS_BY(rps, buffer->size);
        }

        log_debug("Added %zu bytes of TPM2 entropy to the kernel random pool in %s.", done, FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - t, 0));

        r = touch(TPM2_CREDIT_RANDOM_FLAG_PATH);
        if (r < 0)
                log_debug_errno(r, "Failed to touch '" TPM2_CREDIT_RANDOM_FLAG_PATH "', ignoring: %m");

        return 0;
}

static int tpm2_read_public(
                Tpm2Context *c,
                const Tpm2Handle *session,
                const Tpm2Handle *handle,
                TPM2B_PUBLIC **ret_public,
                TPM2B_NAME **ret_name,
                TPM2B_NAME **ret_qname) {

        TSS2_RC rc;

        assert(c);
        assert(handle);

        rc = sym_Esys_ReadPublic(
                        c->esys_context,
                        handle->esys_handle,
                        session ? session->esys_handle : ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ret_public,
                        ret_name,
                        ret_qname);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to read public info: %s", sym_Tss2_RC_Decode(rc));

        return 0;
}

/* Get one of the legacy primary key templates.
 *
 * The legacy templates should only be used for older sealed data that did not use the SRK. Instead of a
 * persistent SRK, a transient key was created to seal the data and then flushed; and the exact same template
 * must be used to recreate the same transient key to unseal the data. The alg parameter must be TPM2_ALG_RSA
 * or TPM2_ALG_ECC. This does not check if the alg is actually supported on this TPM. */
static int tpm2_get_legacy_template(TPMI_ALG_PUBLIC alg, TPMT_PUBLIC *ret_template) {
        /* Do not modify. */
        static const TPMT_PUBLIC legacy_ecc = {
                .type = TPM2_ALG_ECC,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = TPMA_OBJECT_RESTRICTED|TPMA_OBJECT_DECRYPT|TPMA_OBJECT_FIXEDTPM|TPMA_OBJECT_FIXEDPARENT|TPMA_OBJECT_SENSITIVEDATAORIGIN|TPMA_OBJECT_USERWITHAUTH,
                .parameters.eccDetail = {
                        .symmetric = {
                                .algorithm = TPM2_ALG_AES,
                                .keyBits.aes = 128,
                                .mode.aes = TPM2_ALG_CFB,
                        },
                        .scheme.scheme = TPM2_ALG_NULL,
                        .curveID = TPM2_ECC_NIST_P256,
                        .kdf.scheme = TPM2_ALG_NULL,
                },
        };

        /* Do not modify. */
        static const TPMT_PUBLIC legacy_rsa = {
                .type = TPM2_ALG_RSA,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = TPMA_OBJECT_RESTRICTED|TPMA_OBJECT_DECRYPT|TPMA_OBJECT_FIXEDTPM|TPMA_OBJECT_FIXEDPARENT|TPMA_OBJECT_SENSITIVEDATAORIGIN|TPMA_OBJECT_USERWITHAUTH,
                .parameters.rsaDetail = {
                        .symmetric = {
                                .algorithm = TPM2_ALG_AES,
                                .keyBits.aes = 128,
                                .mode.aes = TPM2_ALG_CFB,
                        },
                        .scheme.scheme = TPM2_ALG_NULL,
                        .keyBits = 2048,
                },
        };

        assert(ret_template);

        if (alg == TPM2_ALG_ECC)
                *ret_template = legacy_ecc;
        else if (alg == TPM2_ALG_RSA)
                *ret_template = legacy_rsa;
        else
                return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Unsupported legacy SRK alg: 0x%x", alg);

        return 0;
}

/* Get a Storage Root Key (SRK) template.
 *
 * The SRK template values are recommended by the "TCG TPM v2.0 Provisioning Guidance" document in section
 * 7.5.1 "Storage Primary Key (SRK) Templates", referencing "TCG EK Credential Profile for TPM Family 2.0".
 * The EK Credential Profile version 2.0 provides only a single template each for RSA and ECC, while later EK
 * Credential Profile versions provide more templates, and keep the original templates as "L-1" (for RSA) and
 * "L-2" (for ECC).
 *
 * https://trustedcomputinggroup.org/resource/tcg-tpm-v2-0-provisioning-guidance
 * https://trustedcomputinggroup.org/resource/http-trustedcomputinggroup-org-wp-content-uploads-tcg-ek-credential-profile
 *
 * These templates are only needed to create a new persistent SRK (or a new transient key that is
 * SRK-compatible). Preferably, the TPM should contain a shared SRK located at the reserved shared SRK handle
 * (see TPM2_SRK_HANDLE in tpm2-util.h, and tpm2_get_srk() below).
 *
 * The alg must be TPM2_ALG_RSA or TPM2_ALG_ECC. Returns error if the requested template is not supported on
 * this TPM. Also see tpm2_get_best_srk_template() below. */
int tpm2_get_srk_template(Tpm2Context *c, TPMI_ALG_PUBLIC alg, TPMT_PUBLIC *ret_template) {
        /* The attributes are the same between ECC and RSA templates. This has the changes specified in the
         * Provisioning Guidance document, specifically:
         * TPMA_OBJECT_USERWITHAUTH is added.
         * TPMA_OBJECT_ADMINWITHPOLICY is removed.
         * TPMA_OBJECT_NODA is added. */
        TPMA_OBJECT srk_attributes =
                        TPMA_OBJECT_DECRYPT |
                        TPMA_OBJECT_FIXEDPARENT |
                        TPMA_OBJECT_FIXEDTPM |
                        TPMA_OBJECT_NODA |
                        TPMA_OBJECT_RESTRICTED |
                        TPMA_OBJECT_SENSITIVEDATAORIGIN |
                        TPMA_OBJECT_USERWITHAUTH;

        /* The symmetric configuration is the same between ECC and RSA templates. */
        TPMT_SYM_DEF_OBJECT srk_symmetric = {
                .algorithm = TPM2_ALG_AES,
                .keyBits.aes = 128,
                .mode.aes = TPM2_ALG_CFB,
        };

        /* Both templates have an empty authPolicy as specified by the Provisioning Guidance document. */

        /* From the EK Credential Profile template "L-2". */
        TPMT_PUBLIC srk_ecc = {
                .type = TPM2_ALG_ECC,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = srk_attributes,
                .parameters.eccDetail = {
                        .symmetric = srk_symmetric,
                        .scheme.scheme = TPM2_ALG_NULL,
                        .curveID = TPM2_ECC_NIST_P256,
                        .kdf.scheme = TPM2_ALG_NULL,
                },
        };

        /* From the EK Credential Profile template "L-1". */
        TPMT_PUBLIC srk_rsa = {
                .type = TPM2_ALG_RSA,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = srk_attributes,
                .parameters.rsaDetail = {
                        .symmetric = srk_symmetric,
                        .scheme.scheme = TPM2_ALG_NULL,
                        .keyBits = 2048,
                },
        };

        assert(c);
        assert(ret_template);

        if (alg == TPM2_ALG_ECC) {
                if (!tpm2_supports_alg(c, TPM2_ALG_ECC))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support ECC.");

                if (!tpm2_supports_ecc_curve(c, srk_ecc.parameters.eccDetail.curveID))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support ECC-NIST-P256 curve.");

                if (!tpm2_supports_tpmt_public(c, &srk_ecc))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support SRK ECC template L-2.");

                *ret_template = srk_ecc;
                return 0;
        }

        if (alg == TPM2_ALG_RSA) {
                if (!tpm2_supports_alg(c, TPM2_ALG_RSA))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support RSA.");

                if (!tpm2_supports_tpmt_public(c, &srk_rsa))
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "TPM does not support SRK RSA template L-1.");

                *ret_template = srk_rsa;
                return 0;
        }

        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Unsupported SRK alg: 0x%x.", alg);
}

/* Get the best supported SRK template. ECC is preferred, then RSA. */
int tpm2_get_best_srk_template(Tpm2Context *c, TPMT_PUBLIC *ret_template) {
        if (tpm2_get_srk_template(c, TPM2_ALG_ECC, ret_template) >= 0 ||
            tpm2_get_srk_template(c, TPM2_ALG_RSA, ret_template) >= 0)
                return 0;

        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "TPM does not support either SRK template L-1 (RSA) or L-2 (ECC).");
}

/* Get the TPM2_HANDLE location for the provided Tpm2Handle. */
int tpm2_get_location(Tpm2Context *c, const Tpm2Handle *handle, TPM2_HANDLE *ret_location) {
        TSS2_RC rc;

        assert(c);
        assert(handle);
        assert(ret_location);

        /* Esys_TR_GetTpmHandle was added to tpm2-tss in version 2.4.0. Once we can set a minimum tpm2-tss
         * version of 2.4.0 this check can be removed. */
        if (!sym_Esys_TR_GetTpmHandle)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "libtss2-esys too old, does not include Esys_TR_GetTpmHandle.");

        rc = sym_Esys_TR_GetTpmHandle(c->esys_context, handle->esys_handle, ret_location);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to get handle location: %s", sym_Tss2_RC_Decode(rc));

        return 0;
}

/* Get the Tpm2Handle at the requested location. Returns 1 if found, 0 if the location is empty, or < 0 on
 * error. Also see tpm2_get_srk() below; the SRK is a commonly used persistent Tpm2Handle. */
int tpm2_get_handle(
                Tpm2Context *c,
                TPM2_HANDLE location,
                const Tpm2Handle *session,
                TPM2B_PUBLIC **ret_public,
                TPM2B_NAME **ret_name,
                TPM2B_NAME **ret_qname,
                Tpm2Handle **ret_handle) {

        int r;

        assert(c);

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_esys_handle_from_tpm_handle(c, session, location, &handle);
        if (r < 0)
                return r;
        if (r == 0) { /* No handle at location */
                if (ret_public)
                        *ret_public = NULL;
                if (ret_name)
                        *ret_name = NULL;
                if (ret_qname)
                        *ret_qname = NULL;
                if (ret_handle)
                        *ret_handle = NULL;
                return 0;
        }

        if (ret_public || ret_name || ret_qname) {
                r = tpm2_read_public(c, session, handle, ret_public, ret_name, ret_qname);
                if (r < 0)
                        return r;
        }

        if (ret_handle)
                *ret_handle = TAKE_PTR(handle);

        return 1;
}

/* Get the SRK. Returns 1 if SRK is found, 0 if there is no SRK, or < 0 on error. Also see
 * tpm2_get_or_create_srk() below. */
int tpm2_get_srk(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2B_PUBLIC **ret_public,
                TPM2B_NAME **ret_name,
                TPM2B_NAME **ret_qname,
                Tpm2Handle **ret_handle) {

        return tpm2_get_handle(c, TPM2_SRK_HANDLE, session, ret_public, ret_name, ret_qname, ret_handle);
}

/* Get the SRK, creating one if needed. Returns 0 on success, or < 0 on error. */
int tpm2_get_or_create_srk(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2B_PUBLIC **ret_public,
                TPM2B_NAME **ret_name,
                TPM2B_NAME **ret_qname,
                Tpm2Handle **ret_handle) {

        int r;

        r = tpm2_get_srk(c, session, ret_public, ret_name, ret_qname, ret_handle);
        if (r < 0)
                return r;
        if (r == 1)
                return 0;

        /* No SRK, create and persist one */
        TPM2B_PUBLIC template = { .size = sizeof(TPMT_PUBLIC), };
        r = tpm2_get_best_srk_template(c, &template.publicArea);
        if (r < 0)
                return log_error_errno(r, "Could not get best SRK template: %m");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *transient_handle = NULL;
        r = tpm2_create_primary(
                        c,
                        session,
                        &template,
                        /* sensitive= */ NULL,
                        /* ret_public= */ NULL,
                        &transient_handle);
        if (r < 0)
                return r;

        /* Try to persist the transient SRK we created. No locking needed; if multiple threads are trying to
         * persist SRKs concurrently, only one will succeed (r == 1) while the rest will fail (r == 0). In
         * either case, all threads will get the persistent SRK below. */
        r = tpm2_persist_handle(c, transient_handle, session, TPM2_SRK_HANDLE, /* ret_persistent_handle= */ NULL);
        if (r < 0)
                return r;

        /* The SRK should exist now. */
        r = tpm2_get_srk(c, session, ret_public, ret_name, ret_qname, ret_handle);
        if (r < 0)
                return r;
        if (r == 0)
                /* This should never happen. */
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "SRK we just persisted couldn't be found.");

        return 0;
}

/* Utility functions for TPMS_PCR_SELECTION. */

/* Convert a TPMS_PCR_SELECTION object to a mask. */
uint32_t tpm2_tpms_pcr_selection_to_mask(const TPMS_PCR_SELECTION *s) {
        assert(s);
        assert(s->sizeofSelect <= sizeof(s->pcrSelect));

        uint32_t mask = 0;
        for (unsigned i = 0; i < s->sizeofSelect; i++)
                SET_FLAG(mask, (uint32_t)s->pcrSelect[i] << (i * 8), true);
        return mask;
}

/* Convert a mask and hash alg to a TPMS_PCR_SELECTION object. */
void tpm2_tpms_pcr_selection_from_mask(uint32_t mask, TPMI_ALG_HASH hash_alg, TPMS_PCR_SELECTION *ret) {
        assert(ret);

        /* This is currently hardcoded at 24 PCRs, above. */
        if (!TPM2_PCR_MASK_VALID(mask))
                log_warning("PCR mask selections (%x) out of range, ignoring.",
                            mask & ~((uint32_t)TPM2_PCRS_MASK));

        *ret = (TPMS_PCR_SELECTION){
                .hash = hash_alg,
                .sizeofSelect = TPM2_PCRS_MAX / 8,
                .pcrSelect[0] = mask & 0xff,
                .pcrSelect[1] = (mask >> 8) & 0xff,
                .pcrSelect[2] = (mask >> 16) & 0xff,
        };
}

/* Test if all bits in the mask are set in the TPMS_PCR_SELECTION. */
bool tpm2_tpms_pcr_selection_has_mask(const TPMS_PCR_SELECTION *s, uint32_t mask) {
        assert(s);

        return FLAGS_SET(tpm2_tpms_pcr_selection_to_mask(s), mask);
}

static void tpm2_tpms_pcr_selection_update_mask(TPMS_PCR_SELECTION *s, uint32_t mask, bool b) {
        assert(s);

        tpm2_tpms_pcr_selection_from_mask(UPDATE_FLAG(tpm2_tpms_pcr_selection_to_mask(s), mask, b), s->hash, s);
}

/* Add all PCR selections in the mask. */
void tpm2_tpms_pcr_selection_add_mask(TPMS_PCR_SELECTION *s, uint32_t mask) {
        tpm2_tpms_pcr_selection_update_mask(s, mask, 1);
}

/* Remove all PCR selections in the mask. */
void tpm2_tpms_pcr_selection_sub_mask(TPMS_PCR_SELECTION *s, uint32_t mask) {
        tpm2_tpms_pcr_selection_update_mask(s, mask, 0);
}

/* Add all PCR selections in 'b' to 'a'. Both must have the same hash alg. */
void tpm2_tpms_pcr_selection_add(TPMS_PCR_SELECTION *a, const TPMS_PCR_SELECTION *b) {
        assert(a);
        assert(b);
        assert(a->hash == b->hash);

        tpm2_tpms_pcr_selection_add_mask(a, tpm2_tpms_pcr_selection_to_mask(b));
}

/* Remove all PCR selections in 'b' from 'a'. Both must have the same hash alg. */
void tpm2_tpms_pcr_selection_sub(TPMS_PCR_SELECTION *a, const TPMS_PCR_SELECTION *b) {
        assert(a);
        assert(b);
        assert(a->hash == b->hash);

        tpm2_tpms_pcr_selection_sub_mask(a, tpm2_tpms_pcr_selection_to_mask(b));
}

/* Move all PCR selections in 'b' to 'a'. Both must have the same hash alg. */
void tpm2_tpms_pcr_selection_move(TPMS_PCR_SELECTION *a, TPMS_PCR_SELECTION *b) {
        if (a == b)
                return;

        tpm2_tpms_pcr_selection_add(a, b);
        tpm2_tpms_pcr_selection_from_mask(0, b->hash, b);
}

#define FOREACH_PCR_IN_TPMS_PCR_SELECTION(pcr, tpms)                    \
        _FOREACH_PCR_IN_TPMS_PCR_SELECTION(pcr, tpms, UNIQ)
#define _FOREACH_PCR_IN_TPMS_PCR_SELECTION(pcr, tpms, uniq)             \
        FOREACH_PCR_IN_MASK(pcr, tpm2_tpms_pcr_selection_to_mask(tpms))

#define FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml)    \
        UNIQ_FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml, UNIQ)
#define UNIQ_FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml, uniq) \
        for (TPML_PCR_SELECTION *UNIQ_T(_tpml, uniq) = (TPML_PCR_SELECTION*)(tpml); \
             UNIQ_T(_tpml, uniq); UNIQ_T(_tpml, uniq) = NULL)           \
                _FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, UNIQ_T(_tpml, uniq))
#define _FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml)   \
        for (TPMS_PCR_SELECTION *tpms = tpml->pcrSelections;            \
             (uint32_t)(tpms - tpml->pcrSelections) < tpml->count;      \
             tpms++)

#define FOREACH_PCR_IN_TPML_PCR_SELECTION(pcr, tpms, tpml)              \
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(tpms, tpml)    \
                FOREACH_PCR_IN_TPMS_PCR_SELECTION(pcr, tpms)

char *tpm2_tpms_pcr_selection_to_string(const TPMS_PCR_SELECTION *s) {
        assert(s);

        const char *algstr = strna(tpm2_hash_alg_to_string(s->hash));

        _cleanup_free_ char *mask = tpm2_pcr_mask_to_string(tpm2_tpms_pcr_selection_to_mask(s));
        if (!mask)
                return NULL;

        return strjoin(algstr, "(", mask, ")");
}

size_t tpm2_tpms_pcr_selection_weight(const TPMS_PCR_SELECTION *s) {
        assert(s);

        return popcount(tpm2_tpms_pcr_selection_to_mask(s));
}

/* Utility functions for TPML_PCR_SELECTION. */

/* Remove the (0-based) index entry from 'l', shift all following entries, and update the count. */
static void tpm2_tpml_pcr_selection_remove_index(TPML_PCR_SELECTION *l, uint32_t index) {
        assert(l);
        assert(l->count <= ELEMENTSOF(l->pcrSelections));
        assert(index < l->count);

        size_t s = l->count - (index + 1);
        memmove(&l->pcrSelections[index], &l->pcrSelections[index + 1], s * sizeof(l->pcrSelections[0]));
        l->count--;
}

/* Get a TPMS_PCR_SELECTION from a TPML_PCR_SELECTION for the given hash alg. Returns NULL if there is no
 * entry for the hash alg. This guarantees the returned entry contains all the PCR selections for the given
 * hash alg, which may require modifying the TPML_PCR_SELECTION by removing duplicate entries. */
static TPMS_PCR_SELECTION *tpm2_tpml_pcr_selection_get_tpms_pcr_selection(
                TPML_PCR_SELECTION *l,
                TPMI_ALG_HASH hash_alg) {

        assert(l);
        assert(l->count <= ELEMENTSOF(l->pcrSelections));

        TPMS_PCR_SELECTION *selection = NULL;
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(s, l)
                if (s->hash == hash_alg) {
                        selection = s;
                        break;
                }

        if (!selection)
                return NULL;

        /* Iterate backwards through the entries, removing any other entries for the hash alg. */
        for (uint32_t i = l->count - 1; i > 0; i--) {
                TPMS_PCR_SELECTION *s = &l->pcrSelections[i];

                if (selection == s)
                        break;

                if (s->hash == hash_alg) {
                        tpm2_tpms_pcr_selection_move(selection, s);
                        tpm2_tpml_pcr_selection_remove_index(l, i);
                }
        }

        return selection;
}

/* Convert a TPML_PCR_SELECTION object to a mask. Returns empty mask (i.e. 0) if 'hash_alg' is not in the object. */
uint32_t tpm2_tpml_pcr_selection_to_mask(const TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash_alg) {
        assert(l);

        /* Make a copy, as tpm2_tpml_pcr_selection_get_tpms_pcr_selection() will modify the object if there
         * are multiple entries with the requested hash alg. */
        TPML_PCR_SELECTION lcopy = *l;

        TPMS_PCR_SELECTION *s;
        s = tpm2_tpml_pcr_selection_get_tpms_pcr_selection(&lcopy, hash_alg);
        if (!s)
                return 0;

        return tpm2_tpms_pcr_selection_to_mask(s);
}

/* Convert a mask and hash alg to a TPML_PCR_SELECTION object. */
void tpm2_tpml_pcr_selection_from_mask(uint32_t mask, TPMI_ALG_HASH hash_alg, TPML_PCR_SELECTION *ret) {
        assert(ret);

        TPMS_PCR_SELECTION s;
        tpm2_tpms_pcr_selection_from_mask(mask, hash_alg, &s);

        *ret = (TPML_PCR_SELECTION){
                .count = 1,
                .pcrSelections[0] = s,
        };
}

/* Combine all duplicate (same hash alg) TPMS_PCR_SELECTION entries in 'l'. */
static void tpm2_tpml_pcr_selection_cleanup(TPML_PCR_SELECTION *l) {
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(s, l)
                /* This removes all duplicates for s->hash. */
                (void) tpm2_tpml_pcr_selection_get_tpms_pcr_selection(l, s->hash);
}

/* Add the PCR selections in 's' to the corresponding hash alg TPMS_PCR_SELECTION entry in 'l'. Adds a new
 * TPMS_PCR_SELECTION entry for the hash alg if needed. This may modify the TPML_PCR_SELECTION by combining
 * entries with the same hash alg. */
void tpm2_tpml_pcr_selection_add_tpms_pcr_selection(TPML_PCR_SELECTION *l, const TPMS_PCR_SELECTION *s) {
        assert(l);
        assert(s);

        if (tpm2_tpms_pcr_selection_is_empty(s))
                return;

        TPMS_PCR_SELECTION *selection = tpm2_tpml_pcr_selection_get_tpms_pcr_selection(l, s->hash);
        if (selection) {
                tpm2_tpms_pcr_selection_add(selection, s);
                return;
        }

        /* It's already broken if the count is higher than the array has size for. */
        assert(l->count <= ELEMENTSOF(l->pcrSelections));

        /* If full, the cleanup should result in at least one available entry. */
        if (l->count == ELEMENTSOF(l->pcrSelections))
                tpm2_tpml_pcr_selection_cleanup(l);

        assert(l->count < ELEMENTSOF(l->pcrSelections));
        l->pcrSelections[l->count++] = *s;
}

/* Remove the PCR selections in 's' from the corresponding hash alg TPMS_PCR_SELECTION entry in 'l'. This
 * will combine all entries for 's->hash' in 'l'. */
void tpm2_tpml_pcr_selection_sub_tpms_pcr_selection(TPML_PCR_SELECTION *l, const TPMS_PCR_SELECTION *s) {
        assert(l);
        assert(s);

        if (tpm2_tpms_pcr_selection_is_empty(s))
                return;

        TPMS_PCR_SELECTION *selection = tpm2_tpml_pcr_selection_get_tpms_pcr_selection(l, s->hash);
        if (selection)
                tpm2_tpms_pcr_selection_sub(selection, s);
}

/* Test if all bits in the mask for the hash are set in the TPML_PCR_SELECTION. */
bool tpm2_tpml_pcr_selection_has_mask(const TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash, uint32_t mask) {
        assert(l);

        return FLAGS_SET(tpm2_tpml_pcr_selection_to_mask(l, hash), mask);
}

/* Add the PCR selections in the mask, with the provided hash. */
void tpm2_tpml_pcr_selection_add_mask(TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash, uint32_t mask) {
        TPMS_PCR_SELECTION tpms;

        assert(l);

        tpm2_tpms_pcr_selection_from_mask(mask, hash, &tpms);
        tpm2_tpml_pcr_selection_add_tpms_pcr_selection(l, &tpms);
}

/* Remove the PCR selections in the mask, with the provided hash. */
void tpm2_tpml_pcr_selection_sub_mask(TPML_PCR_SELECTION *l, TPMI_ALG_HASH hash, uint32_t mask) {
        TPMS_PCR_SELECTION tpms;

        assert(l);

        tpm2_tpms_pcr_selection_from_mask(mask, hash, &tpms);
        tpm2_tpml_pcr_selection_sub_tpms_pcr_selection(l, &tpms);
}

/* Add all PCR selections in 'b' to 'a'. */
void tpm2_tpml_pcr_selection_add(TPML_PCR_SELECTION *a, const TPML_PCR_SELECTION *b) {
        assert(a);
        assert(b);

        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(selection_b, (TPML_PCR_SELECTION*) b)
                tpm2_tpml_pcr_selection_add_tpms_pcr_selection(a, selection_b);
}

/* Remove all PCR selections in 'b' from 'a'. */
void tpm2_tpml_pcr_selection_sub(TPML_PCR_SELECTION *a, const TPML_PCR_SELECTION *b) {
        assert(a);
        assert(b);

        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(selection_b, (TPML_PCR_SELECTION*) b)
                tpm2_tpml_pcr_selection_sub_tpms_pcr_selection(a, selection_b);
}

char *tpm2_tpml_pcr_selection_to_string(const TPML_PCR_SELECTION *l) {
        assert(l);

        _cleanup_free_ char *banks = NULL;
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(s, (TPML_PCR_SELECTION*) l) {
                if (tpm2_tpms_pcr_selection_is_empty(s))
                        continue;

                _cleanup_free_ char *str = tpm2_tpms_pcr_selection_to_string(s);
                if (!str || !strextend_with_separator(&banks, ",", str))
                        return NULL;
        }

        return strjoin("[", strempty(banks), "]");
}

size_t tpm2_tpml_pcr_selection_weight(const TPML_PCR_SELECTION *l) {
        assert(l);
        assert(l->count <= ELEMENTSOF(l->pcrSelections));

        size_t weight = 0;
        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(s, l) {
                size_t w = tpm2_tpms_pcr_selection_weight(s);
                assert(weight <= SIZE_MAX - w);
                weight += w;
        }

        return weight;
}

bool TPM2_PCR_VALUE_VALID(const Tpm2PCRValue *pcr_value) {
        int r;

        assert(pcr_value);

        if (!TPM2_PCR_INDEX_VALID(pcr_value->index)) {
                log_debug("PCR index %u invalid.", pcr_value->index);
                return false;
        }

        /* If it contains a value, the value size must match the hash size. */
        if (pcr_value->value.size > 0) {
                r = tpm2_hash_alg_to_size(pcr_value->hash);
                if (r < 0)
                        return false;

                if ((int) pcr_value->value.size != r) {
                        log_debug("PCR hash 0x%" PRIx16 " expected size %d does not match actual size %" PRIu16 ".",
                                  pcr_value->hash, r, pcr_value->value.size);
                        return false;
                }
        }

        return true;
}

/* Verify all entries are valid, and consistent with each other. The requirements for consistency are:
 *
 * 1) all entries must be sorted in ascending order (e.g. using tpm2_sort_pcr_values())
 * 2) all entries must be unique, i.e. there cannot be 2 entries with the same hash and index
 */
bool TPM2_PCR_VALUES_VALID(const Tpm2PCRValue *pcr_values, size_t n_pcr_values) {
        assert(pcr_values || n_pcr_values == 0);

        for (size_t i = 0; i < n_pcr_values; i++) {
                const Tpm2PCRValue *v = &pcr_values[i];

                if (!TPM2_PCR_VALUE_VALID(v))
                        return false;

                if (i == 0)
                        continue;

                const Tpm2PCRValue *l = &pcr_values[i - 1];

                /* Hashes must be sorted in ascending order */
                if (v->hash < l->hash) {
                        log_debug("PCR values not in ascending order, hash %" PRIu16 " is after %" PRIu16 ".",
                                  v->hash, l->hash);
                        return false;
                }

                if (v->hash == l->hash) {
                        /* Indexes (for the same hash) must be sorted in ascending order */
                        if (v->index < l->index) {
                                log_debug("PCR values not in ascending order, hash %" PRIu16 " index %u is after %u.",
                                          v->hash, v->index, l->index);
                                return false;
                        }

                        /* Indexes (for the same hash) must not be duplicates */
                        if (v->index == l->index) {
                                log_debug("PCR values contain duplicates for hash %" PRIu16 " index %u.",
                                          v->hash, v->index);
                                return false;
                        }
                }
        }

        return true;
}

static int cmp_pcr_values(const Tpm2PCRValue *a, const Tpm2PCRValue *b) {
        assert(a);
        assert(b);

        return CMP(a->hash, b->hash) ?: CMP(a->index, b->index);
}

/* Sort the array of Tpm2PCRValue entries in-place. This sorts first in ascending order of hash algorithm
 * (sorting simply by the TPM2 hash algorithm number), and then sorting by pcr index. */
void tpm2_sort_pcr_values(Tpm2PCRValue *pcr_values, size_t n_pcr_values) {
        typesafe_qsort(pcr_values, n_pcr_values, cmp_pcr_values);
}

int tpm2_pcr_values_from_mask(uint32_t mask, TPMI_ALG_HASH hash, Tpm2PCRValue **ret_pcr_values, size_t *ret_n_pcr_values) {
        _cleanup_free_ Tpm2PCRValue *pcr_values = NULL;
        size_t n_pcr_values = 0;

        assert(ret_pcr_values);
        assert(ret_n_pcr_values);

        FOREACH_PCR_IN_MASK(index, mask)
                if (!GREEDY_REALLOC_APPEND(
                                pcr_values,
                                n_pcr_values,
                                &TPM2_PCR_VALUE_MAKE(index, hash, {}),
                                1))
                        return log_oom_debug();

        *ret_pcr_values = TAKE_PTR(pcr_values);
        *ret_n_pcr_values = n_pcr_values;

        return 0;
}

int tpm2_pcr_values_to_mask(const Tpm2PCRValue *pcr_values, size_t n_pcr_values, TPMI_ALG_HASH hash, uint32_t *ret_mask) {
        uint32_t mask = 0;

        assert(pcr_values || n_pcr_values == 0);
        assert(ret_mask);

        if (!TPM2_PCR_VALUES_VALID(pcr_values, n_pcr_values))
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Invalid PCR values.");

        for (size_t i = 0; i < n_pcr_values; i++)
                if (pcr_values[i].hash == hash)
                        SET_BIT(mask, pcr_values[i].index);

        *ret_mask = mask;

        return 0;
}

int tpm2_tpml_pcr_selection_from_pcr_values(
                const Tpm2PCRValue *pcr_values,
                size_t n_pcr_values,
                TPML_PCR_SELECTION *ret_selection,
                TPM2B_DIGEST **ret_values,
                size_t *ret_n_values) {

        TPML_PCR_SELECTION selection = {};
        _cleanup_free_ TPM2B_DIGEST *values = NULL;
        size_t n_values = 0;

        assert(pcr_values || n_pcr_values == 0);

        if (!TPM2_PCR_VALUES_VALID(pcr_values, n_pcr_values))
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "PCR values are not valid.");

        for (size_t i = 0; i < n_pcr_values; i++) {
                unsigned index = pcr_values[i].index;
                TPMI_ALG_HASH hash = pcr_values[i].hash;
                const TPM2B_DIGEST *digest = &pcr_values[i].value;

                tpm2_tpml_pcr_selection_add_mask(&selection, hash, INDEX_TO_MASK(uint32_t, index));

                if (!GREEDY_REALLOC_APPEND(values, n_values, digest, 1))
                        return log_oom_debug();
        }

        if (ret_selection)
                *ret_selection = selection;
        if (ret_values)
                *ret_values = TAKE_PTR(values);
        if (ret_n_values)
                *ret_n_values = n_values;

        return 0;
}

/* Count the number of different hash algorithms for all the entries. */
int tpm2_pcr_values_hash_count(const Tpm2PCRValue *pcr_values, size_t n_pcr_values, size_t *ret_count) {
        TPML_PCR_SELECTION selection;
        int r;

        assert(pcr_values);
        assert(ret_count);

        r = tpm2_tpml_pcr_selection_from_pcr_values(
                        pcr_values,
                        n_pcr_values,
                        &selection,
                        /* ret_values= */ NULL,
                        /* ret_n_values= */ NULL);
        if (r < 0)
                return r;

        *ret_count = selection.count;

        return 0;
}

/* Parse a string argument into a Tpm2PCRValue object.
 *
 * The format is <index>[:hash[=value]] where index is the index number (or name) of the PCR, e.g. 0 (or
 * platform-code), hash is the name of the hash algorithm (e.g. sha256) and value is the hex hash digest
 * value, optionally with a leading 0x. This does not check for validity of the fields. */
int tpm2_pcr_value_from_string(const char *arg, Tpm2PCRValue *ret_pcr_value) {
        Tpm2PCRValue pcr_value = {};
        const char *p = arg;
        int r;

        assert(arg);
        assert(ret_pcr_value);

        _cleanup_free_ char *index = NULL;
        r = extract_first_word(&p, &index, ":", /* flags= */ 0);
        if (r < 1)
                return log_error_errno(r, "Could not parse pcr value '%s': %m", p);

        r = pcr_index_from_string(index);
        if (r < 0)
                return log_error_errno(r, "Invalid pcr index '%s': %m", index);
        pcr_value.index = (unsigned) r;

        if (!isempty(p)) {
                _cleanup_free_ char *hash = NULL;
                r = extract_first_word(&p, &hash, "=", /* flags= */ 0);
                if (r < 1)
                        return log_error_errno(r, "Could not parse pcr hash algorithm '%s': %m", p);

                r = tpm2_hash_alg_from_string(hash);
                if (r < 0)
                        return log_error_errno(r, "Invalid pcr hash algorithm '%s': %m", hash);
                pcr_value.hash = (TPMI_ALG_HASH) r;
        }

        if (!isempty(p)) {
                /* Remove leading 0x if present */
                p = startswith_no_case(p, "0x") ?: p;

                _cleanup_free_ void *buf = NULL;
                size_t buf_size = 0;
                r = unhexmem(p, strlen(p), &buf, &buf_size);
                if (r < 0)
                        return log_error_errno(r, "Invalid pcr hash value '%s': %m", p);

                r = TPM2B_DIGEST_CHECK_SIZE(buf_size);
                if (r < 0)
                        return log_error_errno(r, "PCR hash value size %zu too large.", buf_size);

                pcr_value.value = TPM2B_DIGEST_MAKE(buf, buf_size);
        }

        *ret_pcr_value = pcr_value;

        return 0;
}

/* Return a string for the PCR value. The format is described in tpm2_pcr_value_from_string(). Note that if
 * the hash algorithm is not recognized, neither hash name nor hash digest value is included in the
 * string. This does not check for validity. */
char *tpm2_pcr_value_to_string(const Tpm2PCRValue *pcr_value) {
        _cleanup_free_ char *index = NULL, *value = NULL;
        int r;

        r = asprintf(&index, "%u", pcr_value->index);
        if (r < 0)
                return NULL;

        const char *hash = tpm2_hash_alg_to_string(pcr_value->hash);

        if (hash && pcr_value->value.size > 0) {
                value = hexmem(pcr_value->value.buffer, pcr_value->value.size);
                if (!value)
                        return NULL;
        }

        return strjoin(index, hash ? ":" : "", hash ?: "", value ? "=" : "", value ?: "");
}

/* Parse a string argument into an array of Tpm2PCRValue objects.
 *
 * The format is zero or more entries separated by ',' or '+'. The format of each entry is described in
 * tpm2_pcr_value_from_string(). This does not check for validity of the entries. */
int tpm2_pcr_values_from_string(const char *arg, Tpm2PCRValue **ret_pcr_values, size_t *ret_n_pcr_values) {
        const char *p = arg;
        int r;

        assert(arg);
        assert(ret_pcr_values);
        assert(ret_n_pcr_values);

        _cleanup_free_ Tpm2PCRValue *pcr_values = NULL;
        size_t n_pcr_values = 0;

        for (;;) {
                _cleanup_free_ char *pcr_arg = NULL;
                r = extract_first_word(&p, &pcr_arg, ",+", /* flags= */ 0);
                if (r < 0)
                        return log_error_errno(r, "Could not parse pcr values '%s': %m", p);
                if (r == 0)
                        break;

                Tpm2PCRValue pcr_value;
                r = tpm2_pcr_value_from_string(pcr_arg, &pcr_value);
                if (r < 0)
                        return r;

                if (!GREEDY_REALLOC_APPEND(pcr_values, n_pcr_values, &pcr_value, 1))
                        return log_oom();
        }

        *ret_pcr_values = TAKE_PTR(pcr_values);
        *ret_n_pcr_values = n_pcr_values;

        return 0;
}

/* Return a string representing the array of PCR values. The format is as described in
 * tpm2_pcr_values_from_string(). This does not check for validity. */
char *tpm2_pcr_values_to_string(const Tpm2PCRValue *pcr_values, size_t n_pcr_values) {
        _cleanup_free_ char *s = NULL;

        for (size_t i = 0; i < n_pcr_values; i++) {
                _cleanup_free_ char *pcrstr = tpm2_pcr_value_to_string(&pcr_values[i]);
                if (!pcrstr || !strextend_with_separator(&s, "+", pcrstr))
                        return NULL;
        }

        return s ? TAKE_PTR(s) : strdup("");
}

void tpm2_log_debug_tpml_pcr_selection(const TPML_PCR_SELECTION *l, const char *msg) {
        if (!DEBUG_LOGGING || !l)
                return;

        _cleanup_free_ char *s = tpm2_tpml_pcr_selection_to_string(l);
        log_debug("%s: %s", msg ?: "PCR selection", strna(s));
}

void tpm2_log_debug_pcr_value(const Tpm2PCRValue *pcr_value, const char *msg) {
        if (!DEBUG_LOGGING || !pcr_value)
                return;

        _cleanup_free_ char *s = tpm2_pcr_value_to_string(pcr_value);
        log_debug("%s: %s", msg ?: "PCR value", strna(s));
}

void tpm2_log_debug_buffer(const void *buffer, size_t size, const char *msg) {
        if (!DEBUG_LOGGING || !buffer || size == 0)
                return;

        _cleanup_free_ char *h = hexmem(buffer, size);
        log_debug("%s: %s", msg ?: "Buffer", strna(h));
}

void tpm2_log_debug_digest(const TPM2B_DIGEST *digest, const char *msg) {
        if (digest)
                tpm2_log_debug_buffer(digest->buffer, digest->size, msg ?: "Digest");
}

void tpm2_log_debug_name(const TPM2B_NAME *name, const char *msg) {
        if (name)
                tpm2_log_debug_buffer(name->name, name->size, msg ?: "Name");
}

static int tpm2_get_policy_digest(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2B_DIGEST **ret_policy_digest) {

        TSS2_RC rc;

        if (!DEBUG_LOGGING && !ret_policy_digest)
                return 0;

        assert(c);
        assert(session);

        log_debug("Acquiring policy digest.");

        _cleanup_(Esys_Freep) TPM2B_DIGEST *policy_digest = NULL;
        rc = sym_Esys_PolicyGetDigest(
                        c->esys_context,
                        session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &policy_digest);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to get policy digest from TPM: %s", sym_Tss2_RC_Decode(rc));

        tpm2_log_debug_digest(policy_digest, "Session policy digest");

        if (ret_policy_digest)
                *ret_policy_digest = TAKE_PTR(policy_digest);

        return 0;
}

int tpm2_create_primary(
                Tpm2Context *c,
                const Tpm2Handle *session,
                const TPM2B_PUBLIC *template,
                const TPM2B_SENSITIVE_CREATE *sensitive,
                TPM2B_PUBLIC **ret_public,
                Tpm2Handle **ret_handle) {

        usec_t ts;
        TSS2_RC rc;
        int r;

        assert(c);
        assert(template);

        log_debug("Creating primary key on TPM.");

        ts = now(CLOCK_MONOTONIC);

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        rc = sym_Esys_CreatePrimary(
                        c->esys_context,
                        ESYS_TR_RH_OWNER,
                        session ? session->esys_handle : ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        sensitive ? sensitive : &(TPM2B_SENSITIVE_CREATE) {},
                        template,
                        /* outsideInfo= */ NULL,
                        &(TPML_PCR_SELECTION) {},
                        &handle->esys_handle,
                        &public,
                        /* creationData= */ NULL,
                        /* creationHash= */ NULL,
                        /* creationTicket= */ NULL);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to generate primary key in TPM: %s",
                                       sym_Tss2_RC_Decode(rc));

        log_debug("Successfully created primary key on TPM in %s.",
                  FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - ts, USEC_PER_MSEC));

        if (ret_public)
                *ret_public = TAKE_PTR(public);
        if (ret_handle)
                *ret_handle = TAKE_PTR(handle);

        return 0;
}

/* Create a TPM object. Do not use this to create primary keys, because some HW TPMs refuse to allow that;
 * instead use tpm2_create_primary(). */
int tpm2_create(Tpm2Context *c,
                const Tpm2Handle *parent,
                const Tpm2Handle *session,
                const TPMT_PUBLIC *template,
                const TPMS_SENSITIVE_CREATE *sensitive,
                TPM2B_PUBLIC **ret_public,
                TPM2B_PRIVATE **ret_private) {

        usec_t ts;
        TSS2_RC rc;

        assert(c);
        assert(parent);
        assert(template);

        log_debug("Creating object on TPM.");

        ts = now(CLOCK_MONOTONIC);

        TPM2B_PUBLIC tpm2b_public = {
                .size = sizeof(*template) - sizeof(template->unique),
                .publicArea = *template,
        };

        /* Zero the unique area. */
        zero(tpm2b_public.publicArea.unique);

        TPM2B_SENSITIVE_CREATE tpm2b_sensitive;
        if (sensitive)
                tpm2b_sensitive = (TPM2B_SENSITIVE_CREATE) {
                        .size = sizeof(*sensitive),
                        .sensitive = *sensitive,
                };
        else
                tpm2b_sensitive = (TPM2B_SENSITIVE_CREATE) {};

        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        _cleanup_(Esys_Freep) TPM2B_PRIVATE *private = NULL;
        rc = sym_Esys_Create(
                        c->esys_context,
                        parent->esys_handle,
                        session ? session->esys_handle : ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &tpm2b_sensitive,
                        &tpm2b_public,
                        /* outsideInfo= */ NULL,
                        &(TPML_PCR_SELECTION) {},
                        &private,
                        &public,
                        /* creationData= */ NULL,
                        /* creationHash= */ NULL,
                        /* creationTicket= */ NULL);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to generate object in TPM: %s",
                                       sym_Tss2_RC_Decode(rc));

        log_debug("Successfully created object on TPM in %s.",
                  FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - ts, USEC_PER_MSEC));

        if (ret_public)
                *ret_public = TAKE_PTR(public);
        if (ret_private)
                *ret_private = TAKE_PTR(private);

        return 0;
}

static int tpm2_load(
                Tpm2Context *c,
                const Tpm2Handle *parent,
                const Tpm2Handle *session,
                const TPM2B_PUBLIC *public,
                const TPM2B_PRIVATE *private,
                Tpm2Handle **ret_handle) {

        TSS2_RC rc;
        int r;

        assert(c);
        assert(public);
        assert(private);
        assert(ret_handle);

        log_debug("Loading object into TPM.");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        rc = sym_Esys_Load(
                        c->esys_context,
                        parent ? parent->esys_handle : ESYS_TR_RH_OWNER,
                        session ? session->esys_handle : ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        private,
                        public,
                        &handle->esys_handle);
        if (rc == TPM2_RC_LOCKOUT)
                return log_error_errno(SYNTHETIC_ERRNO(ENOLCK),
                                       "TPM2 device is in dictionary attack lockout mode.");
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to load key into TPM: %s", sym_Tss2_RC_Decode(rc));

        *ret_handle = TAKE_PTR(handle);

        return 0;
}

static int tpm2_load_external(
                Tpm2Context *c,
                const Tpm2Handle *session,
                const TPM2B_PUBLIC *public,
                const TPM2B_SENSITIVE *private,
                Tpm2Handle **ret_handle) {

        TSS2_RC rc;
        int r;

        assert(c);
        assert(ret_handle);

        log_debug("Loading external key into TPM.");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        rc = sym_Esys_LoadExternal(
                        c->esys_context,
                        session ? session->esys_handle : ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        private,
                        public,
#if HAVE_TSS2_ESYS3
                        /* tpm2-tss >= 3.0.0 requires a ESYS_TR_RH_* constant specifying the requested
                         * hierarchy, older versions need TPM2_RH_* instead. */
                        ESYS_TR_RH_OWNER,
#else
                        TPM2_RH_OWNER,
#endif
                        &handle->esys_handle);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to load public key into TPM: %s", sym_Tss2_RC_Decode(rc));

        *ret_handle = TAKE_PTR(handle);

        return 0;
}

/* This calls TPM2_CreateLoaded() directly, without checking if the TPM supports it. Callers should instead
 * use tpm2_create_loaded(). */
static int _tpm2_create_loaded(
                Tpm2Context *c,
                const Tpm2Handle *parent,
                const Tpm2Handle *session,
                const TPMT_PUBLIC *template,
                const TPMS_SENSITIVE_CREATE *sensitive,
                TPM2B_PUBLIC **ret_public,
                TPM2B_PRIVATE **ret_private,
                Tpm2Handle **ret_handle) {

        usec_t ts;
        TSS2_RC rc;
        int r;

        assert(c);
        assert(parent);
        assert(template);

        log_debug("Creating loaded object on TPM.");

        ts = now(CLOCK_MONOTONIC);

        /* Copy the input template and zero the unique area. */
        TPMT_PUBLIC template_copy = *template;
        zero(template_copy.unique);

        TPM2B_TEMPLATE tpm2b_template;
        size_t size = 0;
        rc = sym_Tss2_MU_TPMT_PUBLIC_Marshal(
                        &template_copy,
                        tpm2b_template.buffer,
                        sizeof(tpm2b_template.buffer),
                        &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal public key template: %s", sym_Tss2_RC_Decode(rc));
        assert(size <= UINT16_MAX);
        tpm2b_template.size = size;

        TPM2B_SENSITIVE_CREATE tpm2b_sensitive;
        if (sensitive)
                tpm2b_sensitive = (TPM2B_SENSITIVE_CREATE) {
                        .size = sizeof(*sensitive),
                        .sensitive = *sensitive,
                };
        else
                tpm2b_sensitive = (TPM2B_SENSITIVE_CREATE) {};

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        _cleanup_(Esys_Freep) TPM2B_PRIVATE *private = NULL;
        rc = sym_Esys_CreateLoaded(
                        c->esys_context,
                        parent->esys_handle,
                        session ? session->esys_handle : ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &tpm2b_sensitive,
                        &tpm2b_template,
                        &handle->esys_handle,
                        &private,
                        &public);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to generate loaded object in TPM: %s",
                                       sym_Tss2_RC_Decode(rc));

        log_debug("Successfully created loaded object on TPM in %s.",
                  FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - ts, USEC_PER_MSEC));

        if (ret_public)
                *ret_public = TAKE_PTR(public);
        if (ret_private)
                *ret_private = TAKE_PTR(private);
        if (ret_handle)
                *ret_handle = TAKE_PTR(handle);

        return 0;
}

/* This calls TPM2_CreateLoaded() if the TPM supports it, otherwise it calls TPM2_Create() and TPM2_Load()
 * separately. Do not use this to create primary keys, because some HW TPMs refuse to allow that; instead use
 * tpm2_create_primary(). */
int tpm2_create_loaded(
                Tpm2Context *c,
                const Tpm2Handle *parent,
                const Tpm2Handle *session,
                const TPMT_PUBLIC *template,
                const TPMS_SENSITIVE_CREATE *sensitive,
                TPM2B_PUBLIC **ret_public,
                TPM2B_PRIVATE **ret_private,
                Tpm2Handle **ret_handle) {

        int r;

        if (tpm2_supports_command(c, TPM2_CC_CreateLoaded))
                return _tpm2_create_loaded(c, parent, session, template, sensitive, ret_public, ret_private, ret_handle);

        /* Unfortunately, this TPM doesn't support CreateLoaded (added at spec revision 130) so we need to
         * create and load manually. */
        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        _cleanup_(Esys_Freep) TPM2B_PRIVATE *private = NULL;
        r = tpm2_create(c, parent, session, template, sensitive, &public, &private);
        if (r < 0)
                return r;

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_load(c, parent, session, public, private, &handle);
        if (r < 0)
                return r;

        if (ret_public)
                *ret_public = TAKE_PTR(public);
        if (ret_private)
                *ret_private = TAKE_PTR(private);
        if (ret_handle)
                *ret_handle = TAKE_PTR(handle);

        return 0;
}

static int tpm2_import(
                Tpm2Context *c,
                const Tpm2Handle *parent,
                const Tpm2Handle *session,
                const TPM2B_PUBLIC *public,
                const TPM2B_PRIVATE *private,
                const TPM2B_ENCRYPTED_SECRET *seed,
                const TPM2B_DATA *encryption_key,
                const TPMT_SYM_DEF_OBJECT *symmetric,
                TPM2B_PRIVATE **ret_private) {

        TSS2_RC rc;

        assert(c);
        assert(parent);
        assert(!!encryption_key == !!symmetric);
        assert(public);
        assert(private);
        assert(seed);
        assert(ret_private);

        log_debug("Importing key into TPM.");

        rc = sym_Esys_Import(
                        c->esys_context,
                        parent->esys_handle,
                        session ? session->esys_handle : ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        encryption_key,
                        public,
                        private,
                        seed,
                        symmetric ?: &(TPMT_SYM_DEF_OBJECT){ .algorithm = TPM2_ALG_NULL, },
                        ret_private);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to import key into TPM: %s", sym_Tss2_RC_Decode(rc));

        return 0;
}

/* Read hash values from the specified PCR selection. Provides a Tpm2PCRValue array that contains all
 * requested PCR values, in the order provided by the TPM. Normally, the provided pcr values will match
 * exactly what is in the provided selection, but the TPM may ignore some selected PCRs (for example, if an
 * unimplemented PCR index is requested), in which case those PCRs will be absent from the provided pcr
 * values. */
int tpm2_pcr_read(
                Tpm2Context *c,
                const TPML_PCR_SELECTION *pcr_selection,
                Tpm2PCRValue **ret_pcr_values,
                size_t *ret_n_pcr_values) {

        _cleanup_free_ Tpm2PCRValue *pcr_values = NULL;
        size_t n_pcr_values = 0;
        TSS2_RC rc;

        assert(c);
        assert(pcr_selection);
        assert(ret_pcr_values);
        assert(ret_n_pcr_values);

        TPML_PCR_SELECTION remaining = *pcr_selection;
        while (!tpm2_tpml_pcr_selection_is_empty(&remaining)) {
                _cleanup_(Esys_Freep) TPML_PCR_SELECTION *current_read = NULL;
                _cleanup_(Esys_Freep) TPML_DIGEST *current_values = NULL;

                tpm2_log_debug_tpml_pcr_selection(&remaining, "Reading PCR selection");

                /* Unfortunately, PCR_Read will not return more than 8 values. */
                rc = sym_Esys_PCR_Read(
                                c->esys_context,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                &remaining,
                                NULL,
                                &current_read,
                                &current_values);
                if (rc != TSS2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to read TPM2 PCRs: %s", sym_Tss2_RC_Decode(rc));

                tpm2_log_debug_tpml_pcr_selection(current_read, "Read PCR selection");

                if (tpm2_tpml_pcr_selection_is_empty(current_read)) {
                        log_warning("TPM2 refused to read possibly unimplemented PCRs, ignoring.");
                        break;
                }

                unsigned i = 0;
                FOREACH_PCR_IN_TPML_PCR_SELECTION(index, tpms, current_read) {
                        assert(i < current_values->count);
                        Tpm2PCRValue pcr_value = {
                                .index = index,
                                .hash = tpms->hash,
                                .value = current_values->digests[i++],
                        };

                        tpm2_log_debug_pcr_value(&pcr_value, /* msg= */ NULL);

                        if (!GREEDY_REALLOC_APPEND(pcr_values, n_pcr_values, &pcr_value, 1))
                                return log_oom();
                }
                assert(i == current_values->count);

                tpm2_tpml_pcr_selection_sub(&remaining, current_read);
        }

        tpm2_sort_pcr_values(pcr_values, n_pcr_values);

        if (!TPM2_PCR_VALUES_VALID(pcr_values, n_pcr_values))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "PCR values read from TPM are not valid.");

        *ret_pcr_values = TAKE_PTR(pcr_values);
        *ret_n_pcr_values = n_pcr_values;

        return 0;
}

/* Read the PCR value for each TPM2PCRValue entry in the array that does not have a value set. If all entries
 * have an unset hash (i.e. hash == 0), this first detects the "best" PCR bank to use; otherwise, all entries
 * must have a valid hash set. All entries must have a valid index. If this cannot read a PCR value for all
 * appropriate entries, this returns an error. This does not check the array for validity. */
int tpm2_pcr_read_missing_values(Tpm2Context *c, Tpm2PCRValue *pcr_values, size_t n_pcr_values) {
        TPMI_ALG_HASH pcr_bank = 0;
        int r;

        assert(c);
        assert(pcr_values || n_pcr_values == 0);

        if (n_pcr_values > 0) {
                size_t hash_count;
                r = tpm2_pcr_values_hash_count(pcr_values, n_pcr_values, &hash_count);
                if (r < 0)
                        return log_error_errno(r, "Could not get hash count from pcr values: %m");

                if (hash_count == 1 && pcr_values[0].hash == 0) {
                        uint32_t mask;
                        r = tpm2_pcr_values_to_mask(pcr_values, n_pcr_values, 0, &mask);
                        if (r < 0)
                                return r;

                        r = tpm2_get_best_pcr_bank(c, mask, &pcr_bank);
                        if (r < 0)
                                return r;
                }
        }

        for (size_t i = 0; i < n_pcr_values; i++) {
                Tpm2PCRValue *v = &pcr_values[i];

                if (v->hash == 0)
                        v->hash = pcr_bank;

                if (v->value.size > 0)
                        continue;

                TPML_PCR_SELECTION selection;
                r = tpm2_tpml_pcr_selection_from_pcr_values(v, 1, &selection, NULL, NULL);
                if (r < 0)
                        return r;

                _cleanup_free_ Tpm2PCRValue *read_values = NULL;
                size_t n_read_values;
                r = tpm2_pcr_read(c, &selection, &read_values, &n_read_values);
                if (r < 0)
                        return r;

                if (n_read_values == 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Could not read PCR hash 0x%" PRIu16 " index %u",
                                               v->hash, v->index);

                assert(n_read_values == 1);
                assert(read_values[0].hash == v->hash);
                assert(read_values[0].index == v->index);

                v->value = read_values[0].value;
        }

        return 0;
}

static int tpm2_pcr_mask_good(
                Tpm2Context *c,
                TPMI_ALG_HASH bank,
                uint32_t mask) {

        TPML_PCR_SELECTION selection;
        int r;

        assert(c);

        /* So we have the problem that some systems might have working TPM2 chips, but the firmware doesn't
         * actually measure into them, or only into a suboptimal bank. If so, the PCRs should be all zero or
         * all 0xFF. Detect that, so that we can warn and maybe pick a better bank. */

        tpm2_tpml_pcr_selection_from_mask(mask, bank, &selection);

        _cleanup_free_ Tpm2PCRValue *pcr_values = NULL;
        size_t n_pcr_values;
        r = tpm2_pcr_read(c, &selection, &pcr_values, &n_pcr_values);
        if (r < 0)
                return r;

        /* If at least one of the selected PCR values is something other than all 0x00 or all 0xFF we are happy. */
        for (unsigned i = 0; i < n_pcr_values; i++)
                if (!memeqbyte(0x00, pcr_values[i].value.buffer, pcr_values[i].value.size) &&
                    !memeqbyte(0xFF, pcr_values[i].value.buffer, pcr_values[i].value.size))
                        return true;

        return false;
}

static int tpm2_bank_has24(const TPMS_PCR_SELECTION *selection) {

        assert(selection);

        /* As per https://trustedcomputinggroup.org/wp-content/uploads/TCG_PCClient_PFP_r1p05_v23_pub.pdf a
         * TPM2 on a Client PC must have at least 24 PCRs. If this TPM has less, just skip over it. */
        if (selection->sizeofSelect < TPM2_PCRS_MAX/8) {
                log_debug("Skipping TPM2 PCR bank %s with fewer than 24 PCRs.",
                          strna(tpm2_hash_alg_to_string(selection->hash)));
                return false;
        }

        assert_cc(TPM2_PCRS_MAX % 8 == 0);

        /* It's not enough to check how many PCRs there are, we also need to check that the 24 are
         * enabled for this bank. Otherwise this TPM doesn't qualify. */
        bool valid = true;
        for (size_t j = 0; j < TPM2_PCRS_MAX/8; j++)
                if (selection->pcrSelect[j] != 0xFF) {
                        valid = false;
                        break;
                }

        if (!valid)
                log_debug("TPM2 PCR bank %s has fewer than 24 PCR bits enabled, ignoring.",
                          strna(tpm2_hash_alg_to_string(selection->hash)));

        return valid;
}

int tpm2_get_best_pcr_bank(
                Tpm2Context *c,
                uint32_t pcr_mask,
                TPMI_ALG_HASH *ret) {

        TPMI_ALG_HASH supported_hash = 0, hash_with_valid_pcr = 0;
        int r;

        assert(c);
        assert(ret);

        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(selection, &c->capability_pcrs) {
                TPMI_ALG_HASH hash = selection->hash;
                int good;

                /* For now we are only interested in the SHA1 and SHA256 banks */
                if (!IN_SET(hash, TPM2_ALG_SHA256, TPM2_ALG_SHA1))
                        continue;

                r = tpm2_bank_has24(selection);
                if (r < 0)
                        return r;
                if (!r)
                        continue;

                good = tpm2_pcr_mask_good(c, hash, pcr_mask);
                if (good < 0)
                        return good;

                if (hash == TPM2_ALG_SHA256) {
                        supported_hash = TPM2_ALG_SHA256;
                        if (good) {
                                /* Great, SHA256 is supported and has initialized PCR values, we are done. */
                                hash_with_valid_pcr = TPM2_ALG_SHA256;
                                break;
                        }
                } else {
                        assert(hash == TPM2_ALG_SHA1);

                        if (supported_hash == 0)
                                supported_hash = TPM2_ALG_SHA1;

                        if (good && hash_with_valid_pcr == 0)
                                hash_with_valid_pcr = TPM2_ALG_SHA1;
                }
        }

        /* We preferably pick SHA256, but only if its PCRs are initialized or neither the SHA1 nor the SHA256
         * PCRs are initialized. If SHA256 is not supported but SHA1 is and its PCRs are too, we prefer
         * SHA1.
         *
         * We log at LOG_NOTICE level whenever we end up using the SHA1 bank or when the PCRs we bind to are
         * not initialized. */

        if (hash_with_valid_pcr == TPM2_ALG_SHA256) {
                assert(supported_hash == TPM2_ALG_SHA256);
                log_debug("TPM2 device supports SHA256 PCR bank and SHA256 PCRs are valid, yay!");
                *ret = TPM2_ALG_SHA256;
        } else if (hash_with_valid_pcr == TPM2_ALG_SHA1) {
                if (supported_hash == TPM2_ALG_SHA256)
                        log_notice("TPM2 device supports both SHA1 and SHA256 PCR banks, but only SHA1 PCRs are valid, falling back to SHA1 bank. This reduces the security level substantially.");
                else {
                        assert(supported_hash == TPM2_ALG_SHA1);
                        log_notice("TPM2 device lacks support for SHA256 PCR bank, but SHA1 bank is supported and SHA1 PCRs are valid, falling back to SHA1 bank. This reduces the security level substantially.");
                }

                *ret = TPM2_ALG_SHA1;
        } else if (supported_hash == TPM2_ALG_SHA256) {
                log_notice("TPM2 device supports SHA256 PCR bank but none of the selected PCRs are valid! Firmware apparently did not initialize any of the selected PCRs. Proceeding anyway with SHA256 bank. PCR policy effectively unenforced!");
                *ret = TPM2_ALG_SHA256;
        } else if (supported_hash == TPM2_ALG_SHA1) {
                log_notice("TPM2 device lacks support for SHA256 bank, but SHA1 bank is supported, but none of the selected PCRs are valid! Firmware apparently did not initialize any of the selected PCRs. Proceeding anyway with SHA1 bank. PCR policy effectively unenforced!");
                *ret = TPM2_ALG_SHA1;
        } else
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "TPM2 module supports neither SHA1 nor SHA256 PCR banks, cannot operate.");

        return 0;
}

int tpm2_get_good_pcr_banks(
                Tpm2Context *c,
                uint32_t pcr_mask,
                TPMI_ALG_HASH **ret) {

        _cleanup_free_ TPMI_ALG_HASH *good_banks = NULL, *fallback_banks = NULL;
        size_t n_good_banks = 0, n_fallback_banks = 0;
        int r;

        assert(c);
        assert(ret);

        FOREACH_TPMS_PCR_SELECTION_IN_TPML_PCR_SELECTION(selection, &c->capability_pcrs) {
                TPMI_ALG_HASH hash = selection->hash;

                /* Let's see if this bank is superficially OK, i.e. has at least 24 enabled registers */
                r = tpm2_bank_has24(selection);
                if (r < 0)
                        return r;
                if (!r)
                        continue;

                /* Let's now see if this bank has any of the selected PCRs actually initialized */
                r = tpm2_pcr_mask_good(c, hash, pcr_mask);
                if (r < 0)
                        return r;

                if (n_good_banks + n_fallback_banks >= INT_MAX)
                        return log_error_errno(SYNTHETIC_ERRNO(E2BIG), "Too many good TPM2 banks?");

                if (r) {
                        if (!GREEDY_REALLOC(good_banks, n_good_banks+1))
                                return log_oom();

                        good_banks[n_good_banks++] = hash;
                } else {
                        if (!GREEDY_REALLOC(fallback_banks, n_fallback_banks+1))
                                return log_oom();

                        fallback_banks[n_fallback_banks++] = hash;
                }
        }

        /* Preferably, use the good banks (i.e. the ones the PCR values are actually initialized so
         * far). Otherwise use the fallback banks (i.e. which exist and are enabled, but so far not used. */
        if (n_good_banks > 0) {
                log_debug("Found %zu fully initialized TPM2 banks.", n_good_banks);
                *ret = TAKE_PTR(good_banks);
                return (int) n_good_banks;
        }
        if (n_fallback_banks > 0) {
                log_debug("Found %zu enabled but un-initialized TPM2 banks.", n_fallback_banks);
                *ret = TAKE_PTR(fallback_banks);
                return (int) n_fallback_banks;
        }

        /* No suitable banks found. */
        *ret = NULL;
        return 0;
}

int tpm2_get_good_pcr_banks_strv(
                Tpm2Context *c,
                uint32_t pcr_mask,
                char ***ret) {

#if HAVE_OPENSSL
        _cleanup_free_ TPMI_ALG_HASH *algs = NULL;
        _cleanup_strv_free_ char **l = NULL;
        int n_algs;

        assert(c);
        assert(ret);

        n_algs = tpm2_get_good_pcr_banks(c, pcr_mask, &algs);
        if (n_algs < 0)
                return n_algs;

        for (int i = 0; i < n_algs; i++) {
                _cleanup_free_ char *n = NULL;
                const EVP_MD *implementation;
                const char *salg;

                salg = tpm2_hash_alg_to_string(algs[i]);
                if (!salg)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM2 operates with unknown PCR algorithm, can't measure.");

                implementation = EVP_get_digestbyname(salg);
                if (!implementation)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "TPM2 operates with unsupported PCR algorithm, can't measure.");

                n = strdup(ASSERT_PTR(EVP_MD_name(implementation)));
                if (!n)
                        return log_oom();

                ascii_strlower(n); /* OpenSSL uses uppercase digest names, we prefer them lower case. */

                if (strv_consume(&l, TAKE_PTR(n)) < 0)
                        return log_oom();
        }

        *ret = TAKE_PTR(l);
        return 0;
#else /* HAVE_OPENSSL */
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "OpenSSL support is disabled.");
#endif
}

/* Hash data into the digest.
 *
 * If 'extend' is true, the hashing operation starts with the existing digest hash (and the digest is
 * required to have a hash and its size must be correct). If 'extend' is false, the digest size is
 * initialized to the correct size for 'alg' and the hashing operation does not include any existing digest
 * hash. If 'extend' is false and no data is provided, the digest is initialized to a zero digest.
 *
 * On success, the digest hash will be updated with the hashing operation result and the digest size will be
 * correct for 'alg'.
 *
 * If built without openssl, this only provides SHA256. */
int tpm2_digest_many(
                TPMI_ALG_HASH alg,
                TPM2B_DIGEST *digest,
                const struct iovec data[],
                size_t n_data,
                bool extend) {

        assert(digest);
        assert(data || n_data == 0);

#if HAVE_OPENSSL
        int r;

        const char *digest_alg = tpm2_hash_alg_to_string(alg);
        if (!digest_alg)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Hash algorithm not supported: 0x%04" PRIx16, alg);

        size_t digest_size;
        r = openssl_digest_size(digest_alg, &digest_size);
        if (r < 0)
                return log_error_errno(r, "Could not get digest size: %m");

        if (sizeof(digest->buffer) < digest_size)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Digest hash size %zu too large for TPM2B_DIGEST buffer.",
                                       digest_size);

        _cleanup_free_ struct iovec *data_copy = NULL;
        if (extend) {
                if (digest->size != digest_size)
                        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                               "Digest size 0x%04" PRIx16 ", require 0x%zu",
                                               digest->size, digest_size);

                data_copy = memdup(data, sizeof(data[0]) * n_data);

                struct iovec digest_buf = IOVEC_MAKE(digest->buffer, digest->size);
                size_t n_data_copy = n_data;
                if (!GREEDY_REALLOC_PREPEND(data_copy, n_data_copy, &digest_buf, 1))
                    return log_oom();

                data = data_copy;
                n_data = n_data_copy;
        } else if (n_data == 0) {
                /* If not extending and no data, return zero hash */
                *digest = (TPM2B_DIGEST) {
                        .size = digest_size,
                };

                return 0;
        }

        _cleanup_free_ void *buf = NULL;
        r = openssl_digest_many(digest_alg, data, n_data, &buf, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to calculate digest hash: %m");

        *digest = (TPM2B_DIGEST) {
                .size = digest_size,
        };
        memcpy(digest->buffer, buf, digest_size);
#else
        struct sha256_ctx ctx;

        if (alg != TPM2_ALG_SHA256)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Hash algorithm not supported: 0x%x", alg);

        if (extend && digest->size != SHA256_DIGEST_SIZE)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Digest size 0x%x, require 0x%x",
                                       digest->size, (unsigned)SHA256_DIGEST_SIZE);

        /* Since we're hardcoding SHA256 (for now), we can check this at compile time. */
        assert_cc(sizeof(digest->buffer) >= SHA256_DIGEST_SIZE);

        CLEANUP_ERASE(ctx);

        sha256_init_ctx(&ctx);

        if (extend)
                sha256_process_bytes(digest->buffer, digest->size, &ctx);
        else {
                *digest = (TPM2B_DIGEST){ .size = SHA256_DIGEST_SIZE, };
                if (n_data == 0) /* If not extending and no data, return zero hash */
                        return 0;
        }

        for (size_t i = 0; i < n_data; i++)
                sha256_process_bytes(data[i].iov_base, data[i].iov_len, &ctx);

        sha256_finish_ctx(&ctx, digest->buffer);
#endif

        return 0;
}

/* Same as tpm2_digest_many() but data is contained in TPM2B_DIGEST[]. The digests may be any size digests. */
int tpm2_digest_many_digests(
                TPMI_ALG_HASH alg,
                TPM2B_DIGEST *digest,
                const TPM2B_DIGEST data[],
                size_t n_data,
                bool extend) {

        _cleanup_free_ struct iovec *iovecs = NULL;

        assert(data || n_data == 0);

        iovecs = new(struct iovec, n_data);
        if (!iovecs)
                return log_oom();

        for (size_t i = 0; i < n_data; i++)
                iovecs[i] = IOVEC_MAKE((void*) data[i].buffer, data[i].size);

        return tpm2_digest_many(alg, digest, iovecs, n_data, extend);
}

/* This hashes the provided pin into a digest value, but also verifies that the final byte is not 0, because
 * the TPM specification Part 1 ("Architecture") section Authorization Values (subsection "Authorization Size
 * Convention") states "Trailing octets of zero are to be removed from any string before it is used as an
 * authValue". Since the TPM doesn't know if the auth value is a "string" or just a hash digest, any hash
 * digest that randomly happens to end in 0 must have the final 0(s) trimmed.
 *
 * This is required at 2 points. First, when setting the authValue during creation of new sealed objects, in
 * tpm2_seal(). This only applies to newly created objects, of course.  Second, when using a previously
 * created sealed object that has an authValue set, we use the sealed objects as the session bind key. This
 * requires calling SetAuth so tpm2-tss can correctly calculate the HMAC to use for the encryption session.
 *
 * TPM implementations will perform the trimming for any authValue for existing sealed objects, so the
 * tpm2-tss library must also perform the trimming before HMAC calculation, but it does not yet; this bug is
 * open to add the trimming: https://github.com/tpm2-software/tpm2-tss/issues/2664
 *
 * Until our minimum tpm2-tss version contains a fix for that bug, we must perform the trimming
 * ourselves. Note that since we are trimming, which is exactly what a TPM implementation would do, this will
 * work for both existing objects with a authValue ending in 0(s) as well as new sealed objects we create,
 * which we will trim the 0(s) from before sending to the TPM.
 */
static void tpm2_trim_auth_value(TPM2B_AUTH *auth) {
        bool trimmed = false;

        assert(auth);

        while (auth->size > 0 && auth->buffer[auth->size - 1] == 0) {
                trimmed = true;
                auth->size--;
        }

        if (trimmed)
                log_debug("authValue ends in 0, trimming as required by the TPM2 specification Part 1 section 'HMAC Computation' authValue Note 2.");
}

static int tpm2_get_pin_auth(TPMI_ALG_HASH hash, const char *pin, TPM2B_AUTH *ret_auth) {
        TPM2B_AUTH auth = {};
        int r;

        assert(pin);
        assert(ret_auth);

        r = tpm2_digest_buffer(hash, &auth, pin, strlen(pin), /* extend= */ false);
        if (r < 0)
                return r;

        tpm2_trim_auth_value(&auth);

        *ret_auth = TAKE_STRUCT(auth);

        return 0;
}

static int tpm2_set_auth(Tpm2Context *c, const Tpm2Handle *handle, const char *pin) {
        TPM2B_AUTH auth = {};
        TSS2_RC rc;
        int r;

        assert(c);
        assert(handle);

        if (!pin)
                return 0;

        CLEANUP_ERASE(auth);

        r = tpm2_get_pin_auth(TPM2_ALG_SHA256, pin, &auth);
        if (r < 0)
                return r;

        rc = sym_Esys_TR_SetAuth(c->esys_context, handle->esys_handle, &auth);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to load PIN in TPM: %s", sym_Tss2_RC_Decode(rc));

        return 0;
}

static bool tpm2_is_encryption_session(Tpm2Context *c, const Tpm2Handle *session) {
        TPMA_SESSION flags = 0;
        TSS2_RC rc;

        assert(c);
        assert(session);

        rc = sym_Esys_TRSess_GetAttributes(c->esys_context, session->esys_handle, &flags);
        if (rc != TSS2_RC_SUCCESS)
                return false;

        return (flags & TPMA_SESSION_DECRYPT) && (flags & TPMA_SESSION_ENCRYPT);
}

static int tpm2_make_encryption_session(
                Tpm2Context *c,
                const Tpm2Handle *primary,
                const Tpm2Handle *bind_key,
                Tpm2Handle **ret_session) {

        const TPMA_SESSION sessionAttributes = TPMA_SESSION_DECRYPT | TPMA_SESSION_ENCRYPT |
                        TPMA_SESSION_CONTINUESESSION;
        TSS2_RC rc;
        int r;

        assert(c);
        assert(primary);
        assert(ret_session);

        log_debug("Starting HMAC encryption session.");

        /* Start a salted, unbound HMAC session with a well-known key (e.g. primary key) as tpmKey, which
         * means that the random salt will be encrypted with the well-known key. That way, only the TPM can
         * recover the salt, which is then used for key derivation. */
        _cleanup_(tpm2_handle_freep) Tpm2Handle *session = NULL;
        r = tpm2_handle_new(c, &session);
        if (r < 0)
                return r;

        rc = sym_Esys_StartAuthSession(
                        c->esys_context,
                        primary->esys_handle,
                        bind_key ? bind_key->esys_handle : ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        NULL,
                        TPM2_SE_HMAC,
                        &SESSION_TEMPLATE_SYM_AES_128_CFB,
                        TPM2_ALG_SHA256,
                        &session->esys_handle);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to open session in TPM: %s", sym_Tss2_RC_Decode(rc));

        /* Enable parameter encryption/decryption with AES in CFB mode. Together with HMAC digests (which are
         * always used for sessions), this provides confidentiality, integrity and replay protection for
         * operations that use this session. */
        rc = sym_Esys_TRSess_SetAttributes(c->esys_context, session->esys_handle, sessionAttributes, 0xff);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(
                                SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                "Failed to configure TPM session: %s",
                                sym_Tss2_RC_Decode(rc));

        *ret_session = TAKE_PTR(session);

        return 0;
}

static int tpm2_make_policy_session(
                Tpm2Context *c,
                const Tpm2Handle *primary,
                const Tpm2Handle *encryption_session,
                bool trial,
                Tpm2Handle **ret_session) {

        TPM2_SE session_type = trial ? TPM2_SE_TRIAL : TPM2_SE_POLICY;
        TSS2_RC rc;
        int r;

        assert(c);
        assert(primary);
        assert(encryption_session);
        assert(ret_session);

        if (!tpm2_is_encryption_session(c, encryption_session))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Missing encryption session");

        log_debug("Starting policy session.");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *session = NULL;
        r = tpm2_handle_new(c, &session);
        if (r < 0)
                return r;

        rc = sym_Esys_StartAuthSession(
                        c->esys_context,
                        primary->esys_handle,
                        ESYS_TR_NONE,
                        encryption_session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        NULL,
                        session_type,
                        &SESSION_TEMPLATE_SYM_AES_128_CFB,
                        TPM2_ALG_SHA256,
                        &session->esys_handle);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to open session in TPM: %s", sym_Tss2_RC_Decode(rc));

        *ret_session = TAKE_PTR(session);

        return 0;
}

static int find_signature(
                JsonVariant *v,
                const TPML_PCR_SELECTION *pcr_selection,
                const void *fp,
                size_t fp_size,
                const void *policy,
                size_t policy_size,
                void *ret_signature,
                size_t *ret_signature_size) {

#if HAVE_OPENSSL
        JsonVariant *b, *i;
        const char *k;
        int r;

        /* Searches for a signature blob in the specified JSON object. Search keys are PCR bank, PCR mask,
         * public key, and policy digest. */

        if (!json_variant_is_object(v))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Signature is not a JSON object.");

        uint16_t pcr_bank = pcr_selection->pcrSelections[0].hash;
        uint32_t pcr_mask = tpm2_tpml_pcr_selection_to_mask(pcr_selection, pcr_bank);

        k = tpm2_hash_alg_to_string(pcr_bank);
        if (!k)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Don't know PCR bank %" PRIu16, pcr_bank);

        /* First, find field by bank */
        b = json_variant_by_key(v, k);
        if (!b)
                return log_error_errno(SYNTHETIC_ERRNO(ENXIO), "Signature lacks data for PCR bank '%s'.", k);

        if (!json_variant_is_array(b))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Bank data is not a JSON array.");

        /* Now iterate through all signatures known for this bank */
        JSON_VARIANT_ARRAY_FOREACH(i, b) {
                _cleanup_free_ void *fpj_data = NULL, *polj_data = NULL;
                JsonVariant *maskj, *fpj, *sigj, *polj;
                size_t fpj_size, polj_size;
                uint32_t parsed_mask;

                if (!json_variant_is_object(i))
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Bank data element is not a JSON object");

                /* Check if the PCR mask matches our expectations */
                maskj = json_variant_by_key(i, "pcrs");
                if (!maskj)
                        continue;

                r = tpm2_parse_pcr_json_array(maskj, &parsed_mask);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse JSON PCR mask");

                if (parsed_mask != pcr_mask)
                        continue; /* Not for this PCR mask */

                /* Then check if this is for the public key we operate with */
                fpj = json_variant_by_key(i, "pkfp");
                if (!fpj)
                        continue;

                r = json_variant_unhex(fpj, &fpj_data, &fpj_size);
                if (r < 0)
                        return log_error_errno(r, "Failed to decode fingerprint in JSON data: %m");

                if (memcmp_nn(fp, fp_size, fpj_data, fpj_size) != 0)
                        continue; /* Not for this public key */

                /* Finally, check if this is for the PCR policy we expect this to be */
                polj = json_variant_by_key(i, "pol");
                if (!polj)
                        continue;

                r = json_variant_unhex(polj, &polj_data, &polj_size);
                if (r < 0)
                        return log_error_errno(r, "Failed to decode policy hash JSON data: %m");

                if (memcmp_nn(policy, policy_size, polj_data, polj_size) != 0)
                        continue;

                /* This entry matches all our expectations, now return the signature included in it */
                sigj = json_variant_by_key(i, "sig");
                if (!sigj)
                        continue;

                return json_variant_unbase64(sigj, ret_signature, ret_signature_size);
        }

        return log_error_errno(SYNTHETIC_ERRNO(ENXIO), "Couldn't find signature for this PCR bank, PCR index and public key.");
#else /* HAVE_OPENSSL */
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "OpenSSL support is disabled.");
#endif
}

/* Calculates the "name" of a public key.
 *
 * As specified in TPM2 spec "Part 1: Architecture", a key's "name" is its nameAlg value followed by a hash
 * of its TPM2 public area, all properly marshalled. This allows a key's "name" to be dependent not only on
 * the key fingerprint, but also on the TPM2-specific fields that associated with the key (i.e. all fields in
 * TPMT_PUBLIC). Note that this means an existing key may not change any of its TPMT_PUBLIC fields, since
 * that would also change the key name.
 *
 * Since we (currently) hardcode to always using SHA256 for hashing, this returns an error if the public key
 * nameAlg is not TPM2_ALG_SHA256. */
int tpm2_calculate_name(const TPMT_PUBLIC *public, TPM2B_NAME *ret_name) {
        TSS2_RC rc;
        int r;

        assert(public);
        assert(ret_name);

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        if (public->nameAlg != TPM2_ALG_SHA256)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Unsupported nameAlg: 0x%x",
                                       public->nameAlg);

        _cleanup_free_ uint8_t *buf = NULL;
        size_t size = 0;

        buf = (uint8_t*) new(TPMT_PUBLIC, 1);
        if (!buf)
                return log_oom();

        rc = sym_Tss2_MU_TPMT_PUBLIC_Marshal(public, buf, sizeof(TPMT_PUBLIC), &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal public key: %s", sym_Tss2_RC_Decode(rc));

        TPM2B_DIGEST digest = {};
        r = tpm2_digest_buffer(TPM2_ALG_SHA256, &digest, buf, size, /* extend= */ false);
        if (r < 0)
                return r;

        TPMT_HA ha = {
                .hashAlg = TPM2_ALG_SHA256,
        };
        assert(digest.size <= sizeof(ha.digest.sha256));
        memcpy_safe(ha.digest.sha256, digest.buffer, digest.size);

        TPM2B_NAME name;
        size = 0;
        rc = sym_Tss2_MU_TPMT_HA_Marshal(&ha, name.name, sizeof(name.name), &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal key name: %s", sym_Tss2_RC_Decode(rc));
        name.size = size;

        tpm2_log_debug_name(&name, "Calculated name");

        *ret_name = name;

        return 0;
}

/* Get the "name" of a key from the TPM.
 *
 * The "name" of a key is explained above in tpm2_calculate_name().
 *
 * The handle must reference a key already present in the TPM. It may be either a public key only, or a
 * public/private keypair. */
static int tpm2_get_name(
                Tpm2Context *c,
                const Tpm2Handle *handle,
                TPM2B_NAME **ret_name) {

        _cleanup_(Esys_Freep) TPM2B_NAME *name = NULL;
        TSS2_RC rc;

        assert(c);
        assert(handle);
        assert(ret_name);

        rc = sym_Esys_TR_GetName(c->esys_context, handle->esys_handle, &name);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to get name of public key from TPM: %s", sym_Tss2_RC_Decode(rc));

        tpm2_log_debug_name(name, "Object name");

        *ret_name = TAKE_PTR(name);

        return 0;
}

/* Extend 'digest' with the PolicyAuthValue calculated hash. */
int tpm2_calculate_policy_auth_value(TPM2B_DIGEST *digest) {
        TPM2_CC command = TPM2_CC_PolicyAuthValue;
        TSS2_RC rc;
        int r;

        assert(digest);
        assert(digest->size == SHA256_DIGEST_SIZE);

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        uint8_t buf[sizeof(command)];
        size_t offset = 0;

        rc = sym_Tss2_MU_TPM2_CC_Marshal(command, buf, sizeof(buf), &offset);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal PolicyAuthValue command: %s", sym_Tss2_RC_Decode(rc));

        if (offset != sizeof(command))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Offset 0x%zx wrong after marshalling PolicyAuthValue command", offset);

        r = tpm2_digest_buffer(TPM2_ALG_SHA256, digest, buf, offset, /* extend= */ true);
        if (r < 0)
                return r;

        tpm2_log_debug_digest(digest, "PolicyAuthValue calculated digest");

        return 0;
}

static int tpm2_policy_auth_value(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPM2B_DIGEST **ret_policy_digest) {

        TSS2_RC rc;

        assert(c);
        assert(session);

        log_debug("Adding authValue policy.");

        rc = sym_Esys_PolicyAuthValue(
                        c->esys_context,
                        session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to add authValue policy to TPM: %s",
                                       sym_Tss2_RC_Decode(rc));

        return tpm2_get_policy_digest(c, session, ret_policy_digest);
}

/* Extend 'digest' with the PolicyPCR calculated hash. */
int tpm2_calculate_policy_pcr(
                const Tpm2PCRValue *pcr_values,
                size_t n_pcr_values,
                TPM2B_DIGEST *digest) {

        TPM2_CC command = TPM2_CC_PolicyPCR;
        TSS2_RC rc;
        int r;

        assert(pcr_values || n_pcr_values == 0);
        assert(digest);
        assert(digest->size == SHA256_DIGEST_SIZE);

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        TPML_PCR_SELECTION pcr_selection;
        _cleanup_free_ TPM2B_DIGEST *values = NULL;
        size_t n_values;
        r = tpm2_tpml_pcr_selection_from_pcr_values(pcr_values, n_pcr_values, &pcr_selection, &values, &n_values);
        if (r < 0)
                return log_error_errno(r, "Could not convert PCR values to TPML_PCR_SELECTION: %m");

        TPM2B_DIGEST hash = {};
        r = tpm2_digest_many_digests(TPM2_ALG_SHA256, &hash, values, n_values, /* extend= */ false);
        if (r < 0)
                return r;

        _cleanup_free_ uint8_t *buf = NULL;
        size_t size = 0, maxsize = sizeof(command) + sizeof(pcr_selection);

        buf = malloc(maxsize);
        if (!buf)
                return log_oom();

        rc = sym_Tss2_MU_TPM2_CC_Marshal(command, buf, maxsize, &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal PolicyPCR command: %s", sym_Tss2_RC_Decode(rc));

        rc = sym_Tss2_MU_TPML_PCR_SELECTION_Marshal(&pcr_selection, buf, maxsize, &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal PCR selection: %s", sym_Tss2_RC_Decode(rc));

        struct iovec data[] = {
                IOVEC_MAKE(buf, size),
                IOVEC_MAKE(hash.buffer, hash.size),
        };
        r = tpm2_digest_many(TPM2_ALG_SHA256, digest, data, ELEMENTSOF(data), /* extend= */ true);
        if (r < 0)
                return r;

        tpm2_log_debug_digest(digest, "PolicyPCR calculated digest");

        return 0;
}

static int tpm2_policy_pcr(
                Tpm2Context *c,
                const Tpm2Handle *session,
                const TPML_PCR_SELECTION *pcr_selection,
                TPM2B_DIGEST **ret_policy_digest) {

        TSS2_RC rc;

        assert(c);
        assert(session);
        assert(pcr_selection);

        log_debug("Adding PCR hash policy.");

        rc = sym_Esys_PolicyPCR(
                        c->esys_context,
                        session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        NULL,
                        pcr_selection);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to add PCR policy to TPM: %s", sym_Tss2_RC_Decode(rc));

        return tpm2_get_policy_digest(c, session, ret_policy_digest);
}

/* Extend 'digest' with the PolicyAuthorize calculated hash. */
int tpm2_calculate_policy_authorize(
                const TPM2B_PUBLIC *public,
                const TPM2B_DIGEST *policy_ref,
                TPM2B_DIGEST *digest) {

        TPM2_CC command = TPM2_CC_PolicyAuthorize;
        TSS2_RC rc;
        int r;

        assert(public);
        assert(digest);
        assert(digest->size == SHA256_DIGEST_SIZE);

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support not installed: %m");

        uint8_t buf[sizeof(command)];
        size_t offset = 0;

        rc = sym_Tss2_MU_TPM2_CC_Marshal(command, buf, sizeof(buf), &offset);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal PolicyAuthorize command: %s", sym_Tss2_RC_Decode(rc));

        if (offset != sizeof(command))
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Offset 0x%zx wrong after marshalling PolicyAuthorize command", offset);

        TPM2B_NAME name = {};
        r = tpm2_calculate_name(&public->publicArea, &name);
        if (r < 0)
                return r;

        /* PolicyAuthorize does not use the previous hash value; we must zero and then extend it. */
        zero(digest->buffer);

        struct iovec data[] = {
                IOVEC_MAKE(buf, offset),
                IOVEC_MAKE(name.name, name.size),
        };
        r = tpm2_digest_many(TPM2_ALG_SHA256, digest, data, ELEMENTSOF(data), /* extend= */ true);
        if (r < 0)
                return r;

        /* PolicyAuthorize requires hashing twice; this is either an extension or rehashing. */
        if (policy_ref)
                r = tpm2_digest_many_digests(TPM2_ALG_SHA256, digest, policy_ref, 1, /* extend= */ true);
        else
                r = tpm2_digest_rehash(TPM2_ALG_SHA256, digest);
        if (r < 0)
                return r;

        tpm2_log_debug_digest(digest, "PolicyAuthorize calculated digest");

        return 0;
}

static int tpm2_policy_authorize(
                Tpm2Context *c,
                const Tpm2Handle *session,
                TPML_PCR_SELECTION *pcr_selection,
                const TPM2B_PUBLIC *public,
                const void *fp,
                size_t fp_size,
                JsonVariant *signature_json,
                TPM2B_DIGEST **ret_policy_digest) {

        TSS2_RC rc;
        int r;

        assert(c);
        assert(session);
        assert(pcr_selection);
        assert(public);
        assert(fp && fp_size > 0);

        log_debug("Adding PCR signature policy.");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *pubkey_handle = NULL;
        r = tpm2_load_external(c, NULL, public, NULL, &pubkey_handle);
        if (r < 0)
                return r;

        /* Acquire the "name" of what we just loaded */
        _cleanup_(Esys_Freep) TPM2B_NAME *pubkey_name = NULL;
        r = tpm2_get_name(c, pubkey_handle, &pubkey_name);
        if (r < 0)
                return r;

        /* If we have a signature, proceed with verifying the PCR digest */
        const TPMT_TK_VERIFIED *check_ticket;
        _cleanup_(Esys_Freep) TPMT_TK_VERIFIED *check_ticket_buffer = NULL;
        _cleanup_(Esys_Freep) TPM2B_DIGEST *approved_policy = NULL;
        if (signature_json) {
                r = tpm2_policy_pcr(
                                c,
                                session,
                                pcr_selection,
                                &approved_policy);
                if (r < 0)
                        return r;

                _cleanup_free_ void *signature_raw = NULL;
                size_t signature_size;

                r = find_signature(
                                signature_json,
                                pcr_selection,
                                fp, fp_size,
                                approved_policy->buffer,
                                approved_policy->size,
                                &signature_raw,
                                &signature_size);
                if (r < 0)
                        return r;

                /* TPM2_VerifySignature() will only verify the RSA part of the RSA+SHA256 signature,
                 * hence we need to do the SHA256 part ourselves, first */
                TPM2B_DIGEST signature_hash = *approved_policy;
                r = tpm2_digest_rehash(TPM2_ALG_SHA256, &signature_hash);
                if (r < 0)
                        return r;

                r = TPM2B_PUBLIC_KEY_RSA_CHECK_SIZE(signature_size);
                if (r < 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Signature larger than buffer.");

                TPMT_SIGNATURE policy_signature = {
                        .sigAlg = TPM2_ALG_RSASSA,
                        .signature.rsassa = {
                                .hash = TPM2_ALG_SHA256,
                                .sig = TPM2B_PUBLIC_KEY_RSA_MAKE(signature_raw, signature_size),
                        },
                };

                rc = sym_Esys_VerifySignature(
                                c->esys_context,
                                pubkey_handle->esys_handle,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                ESYS_TR_NONE,
                                &signature_hash,
                                &policy_signature,
                                &check_ticket_buffer);
                if (rc != TSS2_RC_SUCCESS)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to validate signature in TPM: %s", sym_Tss2_RC_Decode(rc));

                check_ticket = check_ticket_buffer;
        } else {
                /* When enrolling, we pass a NULL ticket */
                static const TPMT_TK_VERIFIED check_ticket_null = {
                        .tag = TPM2_ST_VERIFIED,
                        .hierarchy = TPM2_RH_OWNER,
                };

                check_ticket = &check_ticket_null;
        }

        rc = sym_Esys_PolicyAuthorize(
                        c->esys_context,
                        session->esys_handle,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        approved_policy,
                        /* policyRef= */ &(const TPM2B_NONCE) {},
                        pubkey_name,
                        check_ticket);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to push Authorize policy into TPM: %s", sym_Tss2_RC_Decode(rc));

        return tpm2_get_policy_digest(c, session, ret_policy_digest);
}

/* Extend 'digest' with the calculated policy hash. */
int tpm2_calculate_sealing_policy(
                const Tpm2PCRValue *pcr_values,
                size_t n_pcr_values,
                const TPM2B_PUBLIC *public,
                bool use_pin,
                TPM2B_DIGEST *digest) {

        int r;

        assert(pcr_values || n_pcr_values == 0);
        assert(digest);

        if (public) {
                r = tpm2_calculate_policy_authorize(public, NULL, digest);
                if (r < 0)
                        return r;
        }

        if (n_pcr_values > 0) {
                r = tpm2_calculate_policy_pcr(pcr_values, n_pcr_values, digest);
                if (r < 0)
                        return r;
        }

        if (use_pin) {
                r = tpm2_calculate_policy_auth_value(digest);
                if (r < 0)
                        return r;
        }

        return 0;
}

static int tpm2_build_sealing_policy(
                Tpm2Context *c,
                const Tpm2Handle *session,
                uint32_t hash_pcr_mask,
                uint16_t pcr_bank,
                const TPM2B_PUBLIC *public,
                const void *fp,
                size_t fp_size,
                uint32_t pubkey_pcr_mask,
                JsonVariant *signature_json,
                bool use_pin,
                TPM2B_DIGEST **ret_policy_digest) {

        int r;

        assert(c);
        assert(session);
        assert(pubkey_pcr_mask == 0 || public);

        log_debug("Building sealing policy.");

        if ((hash_pcr_mask | pubkey_pcr_mask) != 0) {
                r = tpm2_pcr_mask_good(c, pcr_bank, hash_pcr_mask|pubkey_pcr_mask);
                if (r < 0)
                        return r;
                if (r == 0)
                        log_warning("Selected TPM2 PCRs are not initialized on this system.");
        }

        if (pubkey_pcr_mask != 0) {
                TPML_PCR_SELECTION pcr_selection;
                tpm2_tpml_pcr_selection_from_mask(pubkey_pcr_mask, (TPMI_ALG_HASH)pcr_bank, &pcr_selection);
                r = tpm2_policy_authorize(c, session, &pcr_selection, public, fp, fp_size, signature_json, NULL);
                if (r < 0)
                        return r;
        }

        if (hash_pcr_mask != 0) {
                TPML_PCR_SELECTION pcr_selection;
                tpm2_tpml_pcr_selection_from_mask(hash_pcr_mask, (TPMI_ALG_HASH)pcr_bank, &pcr_selection);
                r = tpm2_policy_pcr(c, session, &pcr_selection, NULL);
                if (r < 0)
                        return r;
        }

        if (use_pin) {
                r = tpm2_policy_auth_value(c, session, NULL);
                if (r < 0)
                        return r;
        }

        r = tpm2_get_policy_digest(c, session, ret_policy_digest);
        if (r < 0)
                return r;

        return 0;
}

#if HAVE_OPENSSL
static int tpm2_ecc_curve_from_openssl_curve_id(int curve_id, TPM2_ECC_CURVE *ret) {
        assert(ret);

        switch (curve_id) {
        case NID_X9_62_prime192v1: *ret = TPM2_ECC_NIST_P192; return 0;
        case NID_secp224r1:        *ret = TPM2_ECC_NIST_P192; return 0;
        case NID_X9_62_prime256v1: *ret = TPM2_ECC_NIST_P256; return 0;
        case NID_secp384r1:        *ret = TPM2_ECC_NIST_P384; return 0;
        case NID_secp521r1:        *ret = TPM2_ECC_NIST_P521; return 0;
        case NID_sm2:              *ret = TPM2_ECC_SM2_P256;  return 0;
        }

        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "Openssl ECC curve id %d not supported.", curve_id);
}

static int tpm2_ecc_curve_to_openssl_curve_id(TPM2_ECC_CURVE curve, int *ret) {
        assert(ret);

        switch (curve) {
        case TPM2_ECC_NIST_P192: *ret = NID_X9_62_prime192v1; return 0;
        case TPM2_ECC_NIST_P224: *ret = NID_secp224r1;        return 0;
        case TPM2_ECC_NIST_P256: *ret = NID_X9_62_prime256v1; return 0;
        case TPM2_ECC_NIST_P384: *ret = NID_secp384r1;        return 0;
        case TPM2_ECC_NIST_P521: *ret = NID_secp521r1;        return 0;
        case TPM2_ECC_SM2_P256:  *ret = NID_sm2;              return 0;
        }

        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "TPM2 ECC curve %u not supported.", curve);
}

#define TPM2_RSA_DEFAULT_EXPONENT UINT32_C(0x10001)

int tpm2_tpm2b_public_to_openssl_pkey(const TPM2B_PUBLIC *public, EVP_PKEY **ret) {
        int r;

        assert(public);
        assert(ret);

        const TPMT_PUBLIC *p = &public->publicArea;
        if (p->type == TPM2_ALG_ECC) {
                int curve_id;
                r = tpm2_ecc_curve_to_openssl_curve_id(p->parameters.eccDetail.curveID, &curve_id);
                if (r < 0)
                        return r;

                const TPMS_ECC_POINT *point = &p->unique.ecc;
                return ecc_pkey_from_curve_x_y(
                                curve_id,
                                point->x.buffer,
                                point->x.size,
                                point->y.buffer,
                                point->y.size,
                                ret);
        }

        if (p->type == TPM2_ALG_RSA) {
                /* TPM specification Part 2 ("Structures") section for TPMS_RSA_PARAMS states "An exponent of
                 * zero indicates that the exponent is the default of 2^16 + 1". */
                uint32_t exponent = htobe32(p->parameters.rsaDetail.exponent ?: TPM2_RSA_DEFAULT_EXPONENT);
                return rsa_pkey_from_n_e(
                                p->unique.rsa.buffer,
                                p->unique.rsa.size,
                                &exponent,
                                sizeof(exponent),
                                ret);
        }

        return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "TPM2 asymmetric algorithm 0x%" PRIx16 " not supported.", p->type);
}

int tpm2_tpm2b_public_from_openssl_pkey(const EVP_PKEY *pkey, TPM2B_PUBLIC *ret) {
        int key_id, r;

        assert(pkey);
        assert(ret);

        TPMT_PUBLIC public = {
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = TPMA_OBJECT_DECRYPT | TPMA_OBJECT_SIGN_ENCRYPT | TPMA_OBJECT_USERWITHAUTH,
                .parameters.asymDetail = {
                        .symmetric.algorithm = TPM2_ALG_NULL,
                        .scheme.scheme = TPM2_ALG_NULL,
                },
        };

#if OPENSSL_VERSION_MAJOR >= 3
        key_id = EVP_PKEY_get_id(pkey);
#else
        key_id = EVP_PKEY_id(pkey);
#endif

        if (key_id == EVP_PKEY_EC) {
                public.type = TPM2_ALG_ECC;

                int curve_id;
                _cleanup_free_ void *x = NULL, *y = NULL;
                size_t x_size, y_size;
                r = ecc_pkey_to_curve_x_y(pkey, &curve_id, &x, &x_size, &y, &y_size);
                if (r < 0)
                        return log_error_errno(r, "Could not get ECC key curve/x/y: %m");

                TPM2_ECC_CURVE curve;
                r = tpm2_ecc_curve_from_openssl_curve_id(curve_id, &curve);
                if (r < 0)
                        return r;

                public.parameters.eccDetail.curveID = curve;

                public.parameters.eccDetail.kdf.scheme = TPM2_ALG_NULL;

                r = TPM2B_ECC_PARAMETER_CHECK_SIZE(x_size);
                if (r < 0)
                        return log_error_errno(r, "ECC key x size %zu too large.", x_size);

                public.unique.ecc.x = TPM2B_ECC_PARAMETER_MAKE(x, x_size);

                r = TPM2B_ECC_PARAMETER_CHECK_SIZE(y_size);
                if (r < 0)
                        return log_error_errno(r, "ECC key y size %zu too large.", y_size);

                public.unique.ecc.y = TPM2B_ECC_PARAMETER_MAKE(y, y_size);
        } else if (key_id == EVP_PKEY_RSA) {
                public.type = TPM2_ALG_RSA;

                _cleanup_free_ void *n = NULL, *e = NULL;
                size_t n_size, e_size;
                r = rsa_pkey_to_n_e(pkey, &n, &n_size, &e, &e_size);
                if (r < 0)
                        return log_error_errno(r, "Could not get RSA key n/e: %m");

                r = TPM2B_PUBLIC_KEY_RSA_CHECK_SIZE(n_size);
                if (r < 0)
                        return log_error_errno(r, "RSA key n size %zu too large.", n_size);

                public.unique.rsa = TPM2B_PUBLIC_KEY_RSA_MAKE(n, n_size);
                public.parameters.rsaDetail.keyBits = n_size * 8;

                if (sizeof(uint32_t) < e_size)
                        return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                               "RSA key e size %zu too large.", e_size);

                uint32_t exponent = 0;
                memcpy((void*) &exponent, e, e_size);
                exponent = be32toh(exponent) >> (32 - e_size * 8);
                if (exponent == TPM2_RSA_DEFAULT_EXPONENT)
                        exponent = 0;
                public.parameters.rsaDetail.exponent = exponent;
        } else
                return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "EVP_PKEY type %d not supported.", key_id);

        *ret = (TPM2B_PUBLIC) {
                .size = sizeof(public),
                .publicArea = public,
        };

        return 0;
}
#endif

int tpm2_tpm2b_public_to_fingerprint(
                const TPM2B_PUBLIC *public,
                void **ret_fingerprint,
                size_t *ret_fingerprint_size) {

#if HAVE_OPENSSL
        int r;

        assert(public);
        assert(ret_fingerprint);
        assert(ret_fingerprint_size);

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;
        r = tpm2_tpm2b_public_to_openssl_pkey(public, &pkey);
        if (r < 0)
                return r;

        /* Hardcode fingerprint to SHA256 */
        return pubkey_fingerprint(pkey, EVP_sha256(), ret_fingerprint, ret_fingerprint_size);
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "OpenSSL support is disabled.");
#endif
}

int tpm2_tpm2b_public_from_pem(const void *pem, size_t pem_size, TPM2B_PUBLIC *ret) {
#if HAVE_OPENSSL
        int r;

        assert(pem);
        assert(ret);

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;
        r = openssl_pkey_from_pem(pem, pem_size, &pkey);
        if (r < 0)
                return r;

        return tpm2_tpm2b_public_from_openssl_pkey(pkey, ret);
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "OpenSSL support is disabled.");
#endif
}

int tpm2_create_blob(
                const TPM2B_PUBLIC *public,
                const TPM2B_PRIVATE *private,
                const TPM2B_ENCRYPTED_SECRET *seed,
                void **ret_blob,
                size_t *ret_blob_size) {

        TSS2_RC rc;

        assert(public);
        assert(private);
        assert(ret_blob);
        assert(ret_blob_size);

        size_t max_size = sizeof(*private) + sizeof(*public);
        if (seed)
                max_size += sizeof(*seed);

        _cleanup_free_ void *blob = malloc(max_size);
        if (!blob)
                return log_oom_debug();

        size_t blob_size = 0;
        rc = sym_Tss2_MU_TPM2B_PRIVATE_Marshal(private, blob, max_size, &blob_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal private key: %s", sym_Tss2_RC_Decode(rc));

        rc = sym_Tss2_MU_TPM2B_PUBLIC_Marshal(public, blob, max_size, &blob_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal public key: %s", sym_Tss2_RC_Decode(rc));

        if (seed) {
                rc = sym_Tss2_MU_TPM2B_ENCRYPTED_SECRET_Marshal(seed, blob, max_size, &blob_size);
                if (rc != TSS2_RC_SUCCESS)
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to marshal encrypted seed: %s", sym_Tss2_RC_Decode(rc));
        }

        *ret_blob = TAKE_PTR(blob);
        *ret_blob_size = blob_size;

        return 0;
}

int tpm2_extract_blob(
                const void *blob,
                size_t blob_size,
                TPM2B_PUBLIC *ret_public,
                TPM2B_PRIVATE *ret_private,
                TPM2B_ENCRYPTED_SECRET *ret_seed) {

        TSS2_RC rc;

        assert(blob);
        assert(ret_public);
        assert(ret_private);
        assert(ret_seed);

        TPM2B_PRIVATE private = {};
        size_t offset = 0;
        rc = sym_Tss2_MU_TPM2B_PRIVATE_Unmarshal(blob, blob_size, &offset, &private);
        if (rc != TSS2_RC_SUCCESS)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to unmarshal private key: %s", sym_Tss2_RC_Decode(rc));

        TPM2B_PUBLIC public = {};
        rc = sym_Tss2_MU_TPM2B_PUBLIC_Unmarshal(blob, blob_size, &offset, &public);
        if (rc != TSS2_RC_SUCCESS)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to unmarshal public key: %s", sym_Tss2_RC_Decode(rc));

        TPM2B_ENCRYPTED_SECRET seed = {};
        if (blob_size > offset) {
                rc = sym_Tss2_MU_TPM2B_ENCRYPTED_SECRET_Unmarshal(blob, blob_size, &offset, &seed);
                if (rc != TSS2_RC_SUCCESS)
                        return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to unmarshal encrypted seed: %s", sym_Tss2_RC_Decode(rc));
        }

        *ret_public = public;
        *ret_private = private;
        *ret_seed = seed;

        return 0;
}

/* Calculate a serialized handle. Once the upstream tpm2-tss library provides an api to do this, we can
 * remove this function. The addition of this functionality in tpm2-tss may be tracked here:
 * https://github.com/tpm2-software/tpm2-tss/issues/2575 */
static int tpm2_calculate_serialize(
                TPM2_HANDLE location,
                const TPM2B_NAME *name,
                const TPM2B_PUBLIC *public,
                void **ret_serialized,
                size_t *ret_serialized_size) {

        TSS2_RC rc;

        assert(name);
        assert(public);
        assert(ret_serialized);
        assert(ret_serialized_size);

        size_t max_size = sizeof(TPM2_HANDLE) + sizeof(TPM2B_NAME) + sizeof(uint32_t) + sizeof(TPM2B_PUBLIC);
        _cleanup_free_ void *serialized = malloc(max_size);
        if (!serialized)
                return log_oom();

        size_t serialized_size = 0;
        rc = sym_Tss2_MU_TPM2_HANDLE_Marshal(location, serialized, max_size, &serialized_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal tpm handle: %s", sym_Tss2_RC_Decode(rc));

        rc = sym_Tss2_MU_TPM2B_NAME_Marshal(name, serialized, max_size, &serialized_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal name: %s", sym_Tss2_RC_Decode(rc));

        /* This is defined (non-publicly) in the tpm2-tss source as IESYSC_KEY_RSRC, to a value of "1". */
        rc = sym_Tss2_MU_UINT32_Marshal(UINT32_C(1), serialized, max_size, &serialized_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal esys resource id: %s", sym_Tss2_RC_Decode(rc));

        rc = sym_Tss2_MU_TPM2B_PUBLIC_Marshal(public, serialized, max_size, &serialized_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal public: %s", sym_Tss2_RC_Decode(rc));

        *ret_serialized = TAKE_PTR(serialized);
        *ret_serialized_size = serialized_size;

        return 0;
}

static int tpm2_serialize(
                Tpm2Context *c,
                const Tpm2Handle *handle,
                void **ret_serialized,
                size_t *ret_serialized_size) {

        TSS2_RC rc;

        assert(c);
        assert(handle);
        assert(ret_serialized);
        assert(ret_serialized_size);

        _cleanup_(Esys_Freep) unsigned char *serialized = NULL;
        size_t size = 0;
        rc = sym_Esys_TR_Serialize(c->esys_context, handle->esys_handle, &serialized, &size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to serialize: %s", sym_Tss2_RC_Decode(rc));

        *ret_serialized = TAKE_PTR(serialized);
        *ret_serialized_size = size;

        return 0;
}

static int tpm2_deserialize(
                Tpm2Context *c,
                const void *serialized,
                size_t serialized_size,
                Tpm2Handle **ret_handle) {

        TSS2_RC rc;
        int r;

        assert(c);
        assert(serialized);
        assert(ret_handle);

        _cleanup_(tpm2_handle_freep) Tpm2Handle *handle = NULL;
        r = tpm2_handle_new(c, &handle);
        if (r < 0)
                return r;

        /* Since this is an existing handle in the TPM we should not implicitly flush it. */
        handle->flush = false;

        rc = sym_Esys_TR_Deserialize(c->esys_context, serialized, serialized_size, &handle->esys_handle);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to deserialize: %s", sym_Tss2_RC_Decode(rc));

        *ret_handle = TAKE_PTR(handle);

        return 0;
}

#if HAVE_OPENSSL

/* KDFa() as defined by the TPM spec. */
static int tpm2_kdfa(
              TPMI_ALG_HASH hash_alg,
              const void *key,
              size_t key_len,
              const char *label,
              const void *context,
              size_t context_len,
              size_t bits,
              void **ret_key,
              size_t *ret_key_len) {

        int r;

        assert(key);
        assert(label);
        assert(context || context_len == 0);
        assert(bits > 0);
        assert(bits <= SIZE_MAX - 7);
        assert(ret_key);
        assert(ret_key_len);

        size_t len = (bits + 7) / 8;

        const char *hash_alg_name = tpm2_hash_alg_to_string(hash_alg);
        if (!hash_alg_name)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Could not get hash name for 0x%" PRIx16 ".", hash_alg);

        _cleanup_free_ void *buf = NULL;
        r = kdf_kb_hmac_derive(
                        "COUNTER",
                        hash_alg_name,
                        key,
                        key_len,
                        label,
                        strlen(label),
                        context,
                        context_len,
                        /* seed= */ NULL,
                        /* seed_len= */ 0,
                        len,
                        &buf);
        if (r < 0)
                return r;

        /* If the number of bits results in a partial byte, the TPM spec requires we zero the unrequested
         * bits in the MSB (i.e. at index 0). From the spec Part 1 ("Architecture") section on Key
         * Derivation Function, specifically KDFa():
         *
         * "The implied return from this function is a sequence of octets with a length equal to (bits + 7) /
         * 8. If bits is not an even multiple of 8, then the returned value occupies the least significant
         * bits of the returned octet array, and the additional, high-order bits in the 0th octet are
         * CLEAR. The unused bits of the most significant octet (MSO) are masked off and not shifted." */
        size_t partial = bits % 8;
        if (partial > 0)
                ((uint8_t*) buf)[0] &= 0xff >> (8 - partial);

        *ret_key = TAKE_PTR(buf);
        *ret_key_len = len;

        return 0;
}

/* KDFe() as defined by the TPM spec. */
static int tpm2_kdfe(
              TPMI_ALG_HASH hash_alg,
              const void *shared_secret,
              size_t shared_secret_len,
              const char *label,
              const void *context_u,
              size_t context_u_size,
              const void *context_v,
              size_t context_v_size,
              size_t bits,
              void **ret_key,
              size_t *ret_key_len) {

        int r;

        assert(shared_secret);
        assert(label);
        assert(context_u);
        assert(context_v);
        assert(bits > 0);
        assert(bits <= SIZE_MAX - 7);
        assert(ret_key);
        assert(ret_key_len);

        size_t len = (bits + 7) / 8;

        const char *hash_alg_name = tpm2_hash_alg_to_string(hash_alg);
        if (!hash_alg_name)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Could not get hash name for 0x%" PRIx16 ".", hash_alg);

        _cleanup_free_ uint8_t *info = malloc(strlen(label) + 1 + context_u_size + context_v_size);
        if (!info)
                return log_oom_debug();

        size_t info_len = 0;
        memcpy(info, label, strlen(label) + 1);
        info_len += strlen(label) + 1;
        memcpy(&info[info_len], context_u, context_u_size);
        info_len += context_u_size;
        memcpy(&info[info_len], context_v, context_v_size);
        info_len += context_v_size;

        _cleanup_free_ void *buf = NULL;
        r = kdf_ss_derive(
                        hash_alg_name,
                        shared_secret,
                        shared_secret_len,
                        /* salt= */ NULL,
                        /* salt_size= */ 0,
                        info,
                        info_len,
                        len,
                        &buf);
        if (r < 0)
                return r;

        *ret_key = TAKE_PTR(buf);
        *ret_key_len = len;

        return 0;
}

static int tpm2_aes_cfb(
             const TPMT_SYM_DEF_OBJECT *symmetric,
             const void *key,
             size_t key_len,
             const void *iv,
             size_t iv_len,
             const struct iovec data[],
             size_t n_data,
             void **ret_encrypted,
             size_t *ret_encrypted_len) {

        assert(symmetric);
        assert(key);
        assert(iv || iv_len == 0);
        assert(data || n_data == 0);
        assert(ret_encrypted);
        assert(ret_encrypted_len);

        if (symmetric->algorithm != TPM2_ALG_AES)
                return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Unsupported symmetric alg: 0x%04" PRIx16, symmetric->algorithm);

        if (symmetric->mode.aes != TPM2_ALG_CFB)
                return log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Unsupported symmetric AES mode: 0x%04" PRIx16, symmetric->mode.aes);

        return openssl_cipher("AES", symmetric->keyBits.aes, "CFB",
                              key, key_len, iv, iv_len, data, n_data, ret_encrypted, ret_encrypted_len);
}

static int tpm2_calculate_seal_public(
                const TPM2B_PUBLIC *parent,
                const TPMA_OBJECT *attributes,
                const TPM2B_DIGEST *policy,
                const TPM2B_DIGEST *seed,
                const void *secret,
                size_t secret_size,
                TPM2B_PUBLIC *ret) {

        int r;

        assert(parent);
        assert(seed);
        assert(secret);
        assert(ret);

        struct iovec data[] = {
                IOVEC_MAKE((void*) seed->buffer, seed->size),
                IOVEC_MAKE((void*) secret, secret_size),
        };
        TPM2B_DIGEST unique;
        r = tpm2_digest_many(
                        parent->publicArea.nameAlg,
                        &unique,
                        data,
                        ELEMENTSOF(data),
                        /* extend= */ false);
        if (r < 0)
                return r;

        *ret = (TPM2B_PUBLIC) {
                .size = sizeof(TPMT_PUBLIC),
                .publicArea = {
                        .type = TPM2_ALG_KEYEDHASH,
                        .nameAlg = parent->publicArea.nameAlg,
                        .objectAttributes = attributes ? *attributes : 0,
                        .authPolicy = policy ? *policy : TPM2B_DIGEST_MAKE(NULL, unique.size),
                        .parameters.keyedHashDetail.scheme.scheme = TPM2_ALG_NULL,
                        .unique.keyedHash = unique,
                },
        };

        return 0;
}

static int tpm2_calculate_seal_private(
                const TPM2B_PUBLIC *parent,
                const TPM2B_NAME *name,
                const char *pin,
                const TPM2B_DIGEST *seed,
                const void *secret,
                size_t secret_size,
                TPM2B_PRIVATE *ret) {

        TSS2_RC rc;
        int r;

        assert(parent);
        assert(name);
        assert(seed);
        assert(secret);
        assert(ret);

        _cleanup_free_ void *storage_key = NULL;
        size_t storage_key_size;
        r = tpm2_kdfa(parent->publicArea.nameAlg,
                      seed->buffer,
                      seed->size,
                      "STORAGE",
                      name->name,
                      name->size,
                      (size_t) parent->publicArea.parameters.asymDetail.symmetric.keyBits.sym,
                      &storage_key,
                      &storage_key_size);
        if (r < 0)
                return log_error_errno(r, "Could not calculate storage key KDFa: %m");

        size_t hash_size = tpm2_hash_alg_to_size(parent->publicArea.nameAlg);
        if (hash_size == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Could not get hash size for 0x%" PRIx16 ".",
                                       parent->publicArea.nameAlg);

        _cleanup_free_ void *integrity_key = NULL;
        size_t integrity_key_size;
        r = tpm2_kdfa(parent->publicArea.nameAlg,
                      seed->buffer,
                      seed->size,
                      "INTEGRITY",
                      /* context= */ NULL,
                      /* n_context= */ 0,
                      hash_size * 8,
                      &integrity_key,
                      &integrity_key_size);
        if (r < 0)
                return log_error_errno(r, "Could not calculate integrity key KDFa: %m");

        TPM2B_AUTH auth = {};
        if (pin) {
                r = tpm2_get_pin_auth(parent->publicArea.nameAlg, pin, &auth);
                if (r < 0)
                        return r;
        }

        TPM2B_SENSITIVE sensitive = {
                .size = sizeof(TPMT_SENSITIVE),
                .sensitiveArea = {
                        .sensitiveType = TPM2_ALG_KEYEDHASH,
                        .authValue = auth,
                        .seedValue = *seed,
                        .sensitive.bits = TPM2B_SENSITIVE_DATA_MAKE(secret, secret_size),
                },
        };

        _cleanup_free_ void *marshalled_sensitive = malloc(sizeof(sensitive));
        if (!marshalled_sensitive)
                return log_oom();

        size_t marshalled_sensitive_size = 0;
        rc = sym_Tss2_MU_TPM2B_SENSITIVE_Marshal(
                        &sensitive,
                        marshalled_sensitive,
                        sizeof(sensitive),
                        &marshalled_sensitive_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal sensitive: %s", sym_Tss2_RC_Decode(rc));

        _cleanup_free_ void *encrypted_sensitive = NULL;
        size_t encrypted_sensitive_size;
        r = tpm2_aes_cfb(
                        &parent->publicArea.parameters.asymDetail.symmetric,
                        storage_key,
                        storage_key_size,
                        /* iv= */ NULL,
                        /* n_iv= */ 0,
                        &IOVEC_MAKE((void*) marshalled_sensitive, marshalled_sensitive_size),
                        1,
                        &encrypted_sensitive,
                        &encrypted_sensitive_size);
        if (r < 0)
                return log_error_errno(r, "CFB encryption failed.");

        const char *hash_alg_name = tpm2_hash_alg_to_string(parent->publicArea.nameAlg);
        if (!hash_alg_name)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Could not get hash name for 0x%" PRIx16 ".",
                                       parent->publicArea.nameAlg);

        _cleanup_free_ void *hmac_buffer = NULL;
        size_t hmac_size = 0;
        struct iovec hmac_data[] = {
                IOVEC_MAKE((void*) encrypted_sensitive, encrypted_sensitive_size),
                IOVEC_MAKE((void*) name->name, name->size),
        };
        r = openssl_hmac_many(
                        hash_alg_name,
                        integrity_key,
                        integrity_key_size,
                        hmac_data,
                        ELEMENTSOF(hmac_data),
                        &hmac_buffer,
                        &hmac_size);
        if (r < 0)
                return log_error_errno(r, "HMAC failed: %m");

        assert(hmac_size <= UINT16_MAX);
        TPM2B_DIGEST outer_hmac = {
                .size = hmac_size,
        };
        memcpy(outer_hmac.buffer, hmac_buffer, hmac_size);

        TPM2B_PRIVATE private = {};
        size_t private_size = 0;
        rc = sym_Tss2_MU_TPM2B_DIGEST_Marshal(
                        &outer_hmac,
                        private.buffer,
                        sizeof(private.buffer),
                        &private_size);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal digest: %s", sym_Tss2_RC_Decode(rc));
        private.size = private_size;

        assert(sizeof(private.buffer) - private.size >= encrypted_sensitive_size);
        memcpy_safe(&private.buffer[private.size], encrypted_sensitive, encrypted_sensitive_size);
        private.size += encrypted_sensitive_size;

        *ret = private;

        return 0;
}

static int tpm2_calculate_seal_rsa_seed(
                const TPM2B_PUBLIC *parent,
                void **ret_seed,
                size_t *ret_seed_size,
                void **ret_encrypted_seed,
                size_t *ret_encrypted_seed_size) {

        int r;

        assert(parent);
        assert(ret_seed);
        assert(ret_seed_size);
        assert(ret_encrypted_seed);
        assert(ret_encrypted_seed_size);

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *parent_pkey = NULL;
        r = tpm2_tpm2b_public_to_openssl_pkey(parent, &parent_pkey);
        if (r < 0)
                return log_debug_errno(r, "Could not convert TPMT_PUBLIC to Openssl PKEY.");

        size_t seed_size = tpm2_hash_alg_to_size(parent->publicArea.nameAlg);
        if (seed_size == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Could not get hash size for 0x%" PRIx16 ".",
                                       parent->publicArea.nameAlg);

        _cleanup_free_ void *seed = malloc(seed_size);
        if (!seed)
                return log_oom_debug();

        r = crypto_random_bytes(seed, seed_size);
        if (r < 0)
                return log_debug_errno(r, "Failed to generate random seed: %m");

        const char *hash_alg_name = tpm2_hash_alg_to_string(parent->publicArea.nameAlg);
        if (!hash_alg_name)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Could not get hash name for 0x%" PRIx16 ".",
                                       parent->publicArea.nameAlg);

        _cleanup_free_ void *encrypted_seed = NULL;
        size_t encrypted_seed_size;
        r = rsa_oaep_encrypt_bytes(
                        parent_pkey,
                        hash_alg_name,
                        "DUPLICATE",
                        seed,
                        seed_size,
                        &encrypted_seed,
                        &encrypted_seed_size);
        if (r < 0)
                return log_debug_errno(r, "Could not RSA-OAEP encrypt random seed: %m");

        *ret_seed = TAKE_PTR(seed);
        *ret_seed_size = seed_size;
        *ret_encrypted_seed = TAKE_PTR(encrypted_seed);
        *ret_encrypted_seed_size = encrypted_seed_size;

        return 0;
}

static int tpm2_calculate_seal_ecc_seed(
                const TPM2B_PUBLIC *parent,
                void **ret_seed,
                size_t *ret_seed_size,
                void **ret_encrypted_seed,
                size_t *ret_encrypted_seed_size) {

        TSS2_RC rc;
        int r;

        assert(parent);
        assert(ret_seed);
        assert(ret_seed_size);
        assert(ret_encrypted_seed);
        assert(ret_encrypted_seed_size);

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *parent_pkey = NULL;
        r = tpm2_tpm2b_public_to_openssl_pkey(parent, &parent_pkey);
        if (r < 0)
                return log_debug_errno(r, "Could not convert TPMT_PUBLIC to Openssl PKEY.");

        _cleanup_(EVP_PKEY_freep) EVP_PKEY *pkey = NULL;
        _cleanup_free_ void *shared_secret = NULL;
        size_t shared_secret_size;
        r = ecc_ecdh(parent_pkey, &pkey, &shared_secret, &shared_secret_size);
        if (r < 0)
                return log_debug_errno(r, "Could not generate ECC shared secret: %m");

        _cleanup_free_ void *x = NULL, *y = NULL;
        size_t x_size, y_size;
        r = ecc_pkey_to_curve_x_y(pkey, NULL, &x, &x_size, &y, &y_size);
        if (r < 0)
                return log_debug_errno(r, "Could not get ECC get x/y: %m");

        TPMS_ECC_POINT point = {
                .x.size = x_size,
                .y.size = y_size,
        };

        if (sizeof(point.x.buffer) < x_size)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "ECC point x too large.");
        memcpy(point.x.buffer, x, x_size);

        if (sizeof(point.y.buffer) < y_size)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "ECC point y too large.");
        memcpy(point.y.buffer, y, y_size);

        _cleanup_free_ void *encrypted_seed = malloc(sizeof(point));
        if (!encrypted_seed)
                return log_oom_debug();

        size_t encrypted_seed_size = 0;
        rc = sym_Tss2_MU_TPMS_ECC_POINT_Marshal(&point, encrypted_seed, sizeof(point), &encrypted_seed_size);
        if (rc != TPM2_RC_SUCCESS)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Failed to marshal ECC point: %s", sym_Tss2_RC_Decode(rc));

        size_t hash_size = tpm2_hash_alg_to_size(parent->publicArea.nameAlg);
        if (hash_size == 0)
                return log_debug_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                       "Could not get hash size for alg 0x%" PRIx16 ".",
                                       parent->publicArea.nameAlg);

        _cleanup_free_ void *seed = NULL;
        size_t seed_size;
        r = tpm2_kdfe(parent->publicArea.nameAlg,
                      shared_secret,
                      shared_secret_size,
                      "DUPLICATE",
                      x,
                      x_size,
                      parent->publicArea.unique.ecc.x.buffer,
                      parent->publicArea.unique.ecc.x.size,
                      hash_size * 8,
                      &seed,
                      &seed_size);
        if (r < 0)
                return log_debug_errno(r, "Could not calculate KDFe: %m");

        *ret_seed = TAKE_PTR(seed);
        *ret_seed_size = seed_size;
        *ret_encrypted_seed = TAKE_PTR(encrypted_seed);
        *ret_encrypted_seed_size = encrypted_seed_size;

        return 0;
}

static int tpm2_calculate_seal_seed(
                const TPM2B_PUBLIC *parent,
                TPM2B_DIGEST *ret_seed,
                TPM2B_ENCRYPTED_SECRET *ret_encrypted_seed) {

        int r;

        assert(parent);
        assert(ret_seed);
        assert(ret_encrypted_seed);

        _cleanup_free_ void *seed = NULL, *encrypted_seed = NULL;
        size_t seed_size, encrypted_seed_size;
        if (parent->publicArea.type == TPM2_ALG_RSA)
                r = tpm2_calculate_seal_rsa_seed(parent, &seed, &seed_size, &encrypted_seed, &encrypted_seed_size);
        else if (parent->publicArea.type == TPM2_ALG_ECC)
                r = tpm2_calculate_seal_ecc_seed(parent, &seed, &seed_size, &encrypted_seed, &encrypted_seed_size);
        else
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                       "Unsupported parent key type 0x%" PRIx16, parent->publicArea.type);
        if (r < 0)
                return log_error_errno(r, "Could not calculate encrypted seed: %m");

        *ret_seed = TPM2B_DIGEST_MAKE(seed, seed_size);
        *ret_encrypted_seed = TPM2B_ENCRYPTED_SECRET_MAKE(encrypted_seed, encrypted_seed_size);

        return 0;
}

int tpm2_calculate_seal(
                TPM2_HANDLE parent_location,
                const TPM2B_PUBLIC *parent_public,
                const TPMA_OBJECT *attributes,
                const void *secret,
                size_t secret_size,
                const TPM2B_DIGEST *policy,
                const char *pin,
                void **ret_secret,
                size_t *ret_secret_size,
                void **ret_blob,
                size_t *ret_blob_size,
                void **ret_serialized_parent,
                size_t *ret_serialized_parent_size) {

        int r;

        assert(parent_public);
        assert(secret || secret_size == 0);
        assert(secret || ret_secret);
        assert(!(secret && ret_secret)); /* Either provide a secret, or we create one, but not both */
        assert(ret_blob);
        assert(ret_blob_size);
        assert(ret_serialized_parent);
        assert(ret_serialized_parent_size);

        if (!IN_SET(TPM2_HANDLE_TYPE(parent_location), TPM2_HT_PERSISTENT, TPM2_HT_NV_INDEX))
                log_warning("Parent handle is not persistent or NV, sealed secret may not be recoverable.");

        _cleanup_(erase_and_freep) void *generated_secret = NULL;
        if (!secret) {
                /* No secret provided, generate a random secret. We use SHA256 digest length, though it can
                 * be up to TPM2_MAX_SEALED_DATA. The secret length is not limited to the nameAlg hash
                 * size. */
                secret_size = TPM2_SHA256_DIGEST_SIZE;
                generated_secret = malloc(secret_size);
                if (!generated_secret)
                        return log_oom();

                r = crypto_random_bytes(generated_secret, secret_size);
                if (r < 0)
                        return log_error_errno(r, "Failed to generate secret key: %m");

                secret = generated_secret;
        }

        if (secret_size > TPM2_MAX_SEALED_DATA)
                return log_error_errno(SYNTHETIC_ERRNO(EOVERFLOW),
                                       "Secret size %zu too large, limit is %d bytes.",
                                       secret_size, TPM2_MAX_SEALED_DATA);

        TPM2B_DIGEST random_seed;
        TPM2B_ENCRYPTED_SECRET seed;
        r = tpm2_calculate_seal_seed(parent_public, &random_seed, &seed);
        if (r < 0)
                return r;

        TPM2B_PUBLIC public;
        r = tpm2_calculate_seal_public(parent_public, attributes, policy, &random_seed, secret, secret_size, &public);
        if (r < 0)
                return r;

        TPM2B_NAME name;
        r = tpm2_calculate_name(&public.publicArea, &name);
        if (r < 0)
                return r;

        TPM2B_PRIVATE private;
        r = tpm2_calculate_seal_private(parent_public, &name, pin, &random_seed, secret, secret_size, &private);
        if (r < 0)
                return r;

        _cleanup_free_ void *blob = NULL;
        size_t blob_size;
        r = tpm2_create_blob(&public, &private, &seed, &blob, &blob_size);
        if (r < 0)
                return log_error_errno(r, "Could not create sealed blob: %m");

        TPM2B_NAME parent_name;
        r = tpm2_calculate_name(&parent_public->publicArea, &parent_name);
        if (r < 0)
                return r;

        _cleanup_free_ void *serialized_parent = NULL;
        size_t serialized_parent_size;
        r = tpm2_calculate_serialize(
                        parent_location,
                        &parent_name,
                        parent_public,
                        &serialized_parent,
                        &serialized_parent_size);
        if (r < 0)
                return r;

        if (ret_secret)
                *ret_secret = TAKE_PTR(generated_secret);
        if (ret_secret_size)
                *ret_secret_size = secret_size;
        *ret_blob = TAKE_PTR(blob);
        *ret_blob_size = blob_size;
        *ret_serialized_parent = TAKE_PTR(serialized_parent);
        *ret_serialized_parent_size = serialized_parent_size;

        return 0;
}

#endif /* HAVE_OPENSSL */

int tpm2_seal(Tpm2Context *c,
              const TPM2B_DIGEST *policy,
              const char *pin,
              void **ret_secret,
              size_t *ret_secret_size,
              void **ret_blob,
              size_t *ret_blob_size,
              uint16_t *ret_primary_alg,
              void **ret_srk_buf,
              size_t *ret_srk_buf_size) {

        uint16_t primary_alg = 0;
        int r;

        assert(ret_secret);
        assert(ret_secret_size);
        assert(ret_blob);
        assert(ret_blob_size);

        /* So here's what we do here: we connect to the TPM2 chip. It persistently contains a "seed" key that
         * is randomized when the TPM2 is first initialized or reset and remains stable across boots. We
         * generate a "primary" key pair derived from that (ECC if possible, RSA as fallback). Given the seed
         * remains fixed this will result in the same key pair whenever we specify the exact same parameters
         * for it. We then create a PCR-bound policy session, which calculates a hash on the current PCR
         * values of the indexes we specify. We then generate a randomized key on the host (which is the key
         * we actually enroll in the LUKS2 keyslots), which we upload into the TPM2, where it is encrypted
         * with the "primary" key, taking the PCR policy session into account. We then download the encrypted
         * key from the TPM2 ("sealing") and marshall it into binary form, which is ultimately placed in the
         * LUKS2 JSON header.
         *
         * The TPM2 "seed" key and "primary" keys never leave the TPM2 chip (and cannot be extracted at
         * all). The random key we enroll in LUKS2 we generate on the host using the Linux random device. It
         * is stored in the LUKS2 JSON only in encrypted form with the "primary" key of the TPM2 chip, thus
         * binding the unlocking to the TPM2 chip. */

        usec_t start = now(CLOCK_MONOTONIC);

        /* We use a keyed hash object (i.e. HMAC) to store the secret key we want to use for unlocking the
         * LUKS2 volume with. We don't ever use for HMAC/keyed hash operations however, we just use it
         * because it's a key type that is universally supported and suitable for symmetric binary blobs. */
        TPMT_PUBLIC hmac_template = {
                .type = TPM2_ALG_KEYEDHASH,
                .nameAlg = TPM2_ALG_SHA256,
                .objectAttributes = TPMA_OBJECT_FIXEDTPM | TPMA_OBJECT_FIXEDPARENT,
                .parameters.keyedHashDetail.scheme.scheme = TPM2_ALG_NULL,
                .unique.keyedHash.size = SHA256_DIGEST_SIZE,
                .authPolicy = policy ? *policy : TPM2B_DIGEST_MAKE(NULL, TPM2_SHA256_DIGEST_SIZE),
        };

        TPMS_SENSITIVE_CREATE hmac_sensitive = {
                .data.size = hmac_template.unique.keyedHash.size,
        };

        CLEANUP_ERASE(hmac_sensitive);

        if (pin) {
                r = tpm2_get_pin_auth(TPM2_ALG_SHA256, pin, &hmac_sensitive.userAuth);
                if (r < 0)
                        return r;
        }

        assert(sizeof(hmac_sensitive.data.buffer) >= hmac_sensitive.data.size);

        (void) tpm2_credit_random(c);

        log_debug("Generating secret key data.");

        r = crypto_random_bytes(hmac_sensitive.data.buffer, hmac_sensitive.data.size);
        if (r < 0)
                return log_error_errno(r, "Failed to generate secret key: %m");

        _cleanup_(tpm2_handle_freep) Tpm2Handle *primary_handle = NULL;
        if (ret_srk_buf) {
                _cleanup_(Esys_Freep) TPM2B_PUBLIC *primary_public = NULL;
                r = tpm2_get_or_create_srk(
                                c,
                                /* session= */ NULL,
                                &primary_public,
                                /* ret_name= */ NULL,
                                /* ret_qname= */ NULL,
                                &primary_handle);
                if (r < 0)
                        return r;

                primary_alg = primary_public->publicArea.type;
        } else {
                /* TODO: force all callers to provide ret_srk_buf, so we can stop sealing with the legacy templates. */
                primary_alg = TPM2_ALG_ECC;

                TPM2B_PUBLIC template = { .size = sizeof(TPMT_PUBLIC), };
                r = tpm2_get_legacy_template(primary_alg, &template.publicArea);
                if (r < 0)
                        return log_error_errno(r, "Could not get legacy ECC template: %m");

                if (!tpm2_supports_tpmt_public(c, &template.publicArea)) {
                        primary_alg = TPM2_ALG_RSA;

                        r = tpm2_get_legacy_template(primary_alg, &template.publicArea);
                        if (r < 0)
                                return log_error_errno(r, "Could not get legacy RSA template: %m");

                        if (!tpm2_supports_tpmt_public(c, &template.publicArea))
                                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                                       "TPM does not support either ECC or RSA legacy template.");
                }

                r = tpm2_create_primary(
                                c,
                                /* session= */ NULL,
                                &template,
                                /* sensitive= */ NULL,
                                /* ret_public= */ NULL,
                                &primary_handle);
                if (r < 0)
                        return r;
        }

        _cleanup_(tpm2_handle_freep) Tpm2Handle *encryption_session = NULL;
        r = tpm2_make_encryption_session(c, primary_handle, /* bind_key= */ NULL, &encryption_session);
        if (r < 0)
                return r;

        _cleanup_(Esys_Freep) TPM2B_PUBLIC *public = NULL;
        _cleanup_(Esys_Freep) TPM2B_PRIVATE *private = NULL;
        r = tpm2_create(c, primary_handle, encryption_session, &hmac_template, &hmac_sensitive, &public, &private);
        if (r < 0)
                return r;

        _cleanup_(erase_and_freep) void *secret = NULL;
        secret = memdup(hmac_sensitive.data.buffer, hmac_sensitive.data.size);
        if (!secret)
                return log_oom();

        log_debug("Marshalling private and public part of HMAC key.");

        _cleanup_free_ void *blob = NULL;
        size_t blob_size;
        r = tpm2_create_blob(public, private, /* seed= */ NULL, &blob, &blob_size);
        if (r < 0)
                return log_error_errno(r, "Could not create sealed blob: %m");

        if (DEBUG_LOGGING)
                log_debug("Completed TPM2 key sealing in %s.", FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - start, 1));

        _cleanup_free_ void *srk_buf = NULL;
        size_t srk_buf_size = 0;
        if (ret_srk_buf) {
                _cleanup_(Esys_Freep) void *tmp = NULL;
                r = tpm2_serialize(c, primary_handle, &tmp, &srk_buf_size);
                if (r < 0)
                        return r;

                /*
                 * make a copy since we don't want the caller to understand that
                 * ESYS allocated the pointer. It would make tracking what deallocator
                 * to use for srk_buf in which context a PITA.
                 */
                srk_buf = memdup(tmp, srk_buf_size);
                if (!srk_buf)
                        return log_oom();

                *ret_srk_buf = TAKE_PTR(srk_buf);
                *ret_srk_buf_size = srk_buf_size;
        }

        *ret_secret = TAKE_PTR(secret);
        *ret_secret_size = hmac_sensitive.data.size;
        *ret_blob = TAKE_PTR(blob);
        *ret_blob_size = blob_size;

        if (ret_primary_alg)
                *ret_primary_alg = primary_alg;

        return 0;
}

#define RETRY_UNSEAL_MAX 30u

int tpm2_unseal(const char *device,
                uint32_t hash_pcr_mask,
                uint16_t pcr_bank,
                const void *pubkey,
                size_t pubkey_size,
                uint32_t pubkey_pcr_mask,
                JsonVariant *signature,
                const char *pin,
                uint16_t primary_alg,
                const void *blob,
                size_t blob_size,
                const void *known_policy_hash,
                size_t known_policy_hash_size,
                const void *srk_buf,
                size_t srk_buf_size,
                void **ret_secret,
                size_t *ret_secret_size) {

        TSS2_RC rc;
        int r;

        assert(blob);
        assert(blob_size > 0);
        assert(known_policy_hash_size == 0 || known_policy_hash);
        assert(pubkey_size == 0 || pubkey);
        assert(ret_secret);
        assert(ret_secret_size);

        assert(TPM2_PCR_MASK_VALID(hash_pcr_mask));
        assert(TPM2_PCR_MASK_VALID(pubkey_pcr_mask));

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support is not installed.");

        /* So here's what we do here: We connect to the TPM2 chip. As we do when sealing we generate a
         * "primary" key on the TPM2 chip, with the same parameters as well as a PCR-bound policy session.
         * Given we pass the same parameters, this will result in the same "primary" key, and same policy
         * hash (the latter of course, only if the PCR values didn't change in between). We unmarshal the
         * encrypted key we stored in the LUKS2 JSON token header and upload it into the TPM2, where it is
         * decrypted if the seed and the PCR policy were right ("unsealing"). We then download the result,
         * and use it to unlock the LUKS2 volume. */

        usec_t start = now(CLOCK_MONOTONIC);

        TPM2B_PUBLIC public;
        TPM2B_PRIVATE private;
        TPM2B_ENCRYPTED_SECRET seed = {};
        r = tpm2_extract_blob(blob, blob_size, &public, &private, &seed);
        if (r < 0)
                return log_error_errno(r, "Could not extract parts from blob: %m");

        _cleanup_(tpm2_context_unrefp) Tpm2Context *c = NULL;
        r = tpm2_context_new(device, &c);
        if (r < 0)
                return r;

        /* Older code did not save the pcr_bank, and unsealing needed to detect the best pcr bank to use,
         * so we need to handle that legacy situation. */
        if (pcr_bank == UINT16_MAX) {
                r = tpm2_get_best_pcr_bank(c, hash_pcr_mask|pubkey_pcr_mask, &pcr_bank);
                if (r < 0)
                        return r;
        }

        _cleanup_(tpm2_handle_freep) Tpm2Handle *primary_handle = NULL;
        if (srk_buf) {
                r = tpm2_deserialize(c, srk_buf, srk_buf_size, &primary_handle);
                if (r < 0)
                        return r;
        } else if (primary_alg != 0) {
                TPM2B_PUBLIC template = { .size = sizeof(TPMT_PUBLIC), };
                r = tpm2_get_legacy_template(primary_alg, &template.publicArea);
                if (r < 0)
                        return log_error_errno(r, "Could not get legacy template: %m");

                r = tpm2_create_primary(
                                c,
                                /* session= */ NULL,
                                &template,
                                /* sensitive= */ NULL,
                                /* ret_public= */ NULL,
                                &primary_handle);
                if (r < 0)
                        return r;
        } else
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "No SRK or primary alg provided.");

        if (seed.size > 0) {
                /* This is a calculated (or duplicated) sealed object, and must be imported. */
                _cleanup_free_ TPM2B_PRIVATE *imported_private = NULL;
                r = tpm2_import(c,
                                primary_handle,
                                /* session= */ NULL,
                                &public,
                                &private,
                                &seed,
                                /* encryption_key= */ NULL,
                                /* symmetric= */ NULL,
                                &imported_private);
                if (r < 0)
                        return r;

                private = *imported_private;
        }

        log_debug("Loading HMAC key into TPM.");

        /*
         * Nothing sensitive on the bus, no need for encryption. Even if an attacker
         * gives you back a different key, the session initiation will fail. In the
         * SRK model, the tpmKey is verified. In the non-srk model, with pin, the bindKey
         * provides protections.
         */
        _cleanup_(tpm2_handle_freep) Tpm2Handle *hmac_key = NULL;
        r = tpm2_load(c, primary_handle, NULL, &public, &private, &hmac_key);
        if (r < 0)
                return r;

        TPM2B_PUBLIC pubkey_tpm2b;
        _cleanup_free_ void *fp = NULL;
        size_t fp_size = 0;
        if (pubkey) {
                r = tpm2_tpm2b_public_from_pem(pubkey, pubkey_size, &pubkey_tpm2b);
                if (r < 0)
                        return log_error_errno(r, "Could not create TPMT_PUBLIC: %m");

                r = tpm2_tpm2b_public_to_fingerprint(&pubkey_tpm2b, &fp, &fp_size);
                if (r < 0)
                        return log_error_errno(r, "Could not get key fingerprint: %m");
        }

        /*
         * if a pin is set for the seal object, use it to bind the session
         * key to that object. This prevents active bus interposers from
         * faking a TPM and seeing the unsealed value. An active interposer
         * could fake a TPM, satisfying the encrypted session, and just
         * forward everything to the *real* TPM.
         */
        r = tpm2_set_auth(c, hmac_key, pin);
        if (r < 0)
                return r;

        _cleanup_(tpm2_handle_freep) Tpm2Handle *encryption_session = NULL;
        r = tpm2_make_encryption_session(c, primary_handle, hmac_key, &encryption_session);
        if (r < 0)
                return r;

        _cleanup_(Esys_Freep) TPM2B_SENSITIVE_DATA* unsealed = NULL;
        for (unsigned i = RETRY_UNSEAL_MAX;; i--) {
                _cleanup_(tpm2_handle_freep) Tpm2Handle *policy_session = NULL;
                _cleanup_(Esys_Freep) TPM2B_DIGEST *policy_digest = NULL;
                r = tpm2_make_policy_session(
                                c,
                                primary_handle,
                                encryption_session,
                                /* trial= */ false,
                                &policy_session);
                if (r < 0)
                        return r;

                r = tpm2_build_sealing_policy(
                                c,
                                policy_session,
                                hash_pcr_mask,
                                pcr_bank,
                                pubkey ? &pubkey_tpm2b : NULL,
                                fp, fp_size,
                                pubkey_pcr_mask,
                                signature,
                                !!pin,
                                &policy_digest);
                if (r < 0)
                        return r;

                /* If we know the policy hash to expect, and it doesn't match, we can shortcut things here, and not
                 * wait until the TPM2 tells us to go away. */
                if (known_policy_hash_size > 0 &&
                        memcmp_nn(policy_digest->buffer, policy_digest->size, known_policy_hash, known_policy_hash_size) != 0)
                                return log_error_errno(SYNTHETIC_ERRNO(EPERM),
                                                       "Current policy digest does not match stored policy digest, cancelling "
                                                       "TPM2 authentication attempt.");

                log_debug("Unsealing HMAC key.");

                rc = sym_Esys_Unseal(
                                c->esys_context,
                                hmac_key->esys_handle,
                                policy_session->esys_handle,
                                encryption_session->esys_handle, /* use HMAC session to enable parameter encryption */
                                ESYS_TR_NONE,
                                &unsealed);
                if (rc == TSS2_RC_SUCCESS)
                        break;
                if (rc != TPM2_RC_PCR_CHANGED || i == 0)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                               "Failed to unseal HMAC key in TPM: %s", sym_Tss2_RC_Decode(rc));
                log_debug("A PCR value changed during the TPM2 policy session, restarting HMAC key unsealing (%u tries left).", i);
        }

        _cleanup_(erase_and_freep) char *secret = NULL;
        secret = memdup(unsealed->buffer, unsealed->size);
        explicit_bzero_safe(unsealed->buffer, unsealed->size);
        if (!secret)
                return log_oom();

        if (DEBUG_LOGGING)
                log_debug("Completed TPM2 key unsealing in %s.", FORMAT_TIMESPAN(now(CLOCK_MONOTONIC) - start, 1));

        *ret_secret = TAKE_PTR(secret);
        *ret_secret_size = unsealed->size;

        return 0;
}

#endif

int tpm2_list_devices(void) {
#if HAVE_TPM2
        _cleanup_(table_unrefp) Table *t = NULL;
        _cleanup_closedir_ DIR *d = NULL;
        int r;

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support is not installed.");

        t = table_new("path", "device", "driver");
        if (!t)
                return log_oom();

        d = opendir("/sys/class/tpmrm");
        if (!d) {
                log_full_errno(errno == ENOENT ? LOG_DEBUG : LOG_ERR, errno, "Failed to open /sys/class/tpmrm: %m");
                if (errno != ENOENT)
                        return -errno;
        } else {
                for (;;) {
                        _cleanup_free_ char *device_path = NULL, *device = NULL, *driver_path = NULL, *driver = NULL, *node = NULL;
                        struct dirent *de;

                        de = readdir_no_dot(d);
                        if (!de)
                                break;

                        device_path = path_join("/sys/class/tpmrm", de->d_name, "device");
                        if (!device_path)
                                return log_oom();

                        r = readlink_malloc(device_path, &device);
                        if (r < 0)
                                log_debug_errno(r, "Failed to read device symlink %s, ignoring: %m", device_path);
                        else {
                                driver_path = path_join(device_path, "driver");
                                if (!driver_path)
                                        return log_oom();

                                r = readlink_malloc(driver_path, &driver);
                                if (r < 0)
                                        log_debug_errno(r, "Failed to read driver symlink %s, ignoring: %m", driver_path);
                        }

                        node = path_join("/dev", de->d_name);
                        if (!node)
                                return log_oom();

                        r = table_add_many(
                                        t,
                                        TABLE_PATH, node,
                                        TABLE_STRING, device ? last_path_component(device) : NULL,
                                        TABLE_STRING, driver ? last_path_component(driver) : NULL);
                        if (r < 0)
                                return table_log_add_error(r);
                }
        }

        if (table_get_rows(t) <= 1) {
                log_info("No suitable TPM2 devices found.");
                return 0;
        }

        r = table_print(t, stdout);
        if (r < 0)
                return log_error_errno(r, "Failed to show device table: %m");

        return 0;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "TPM2 not supported on this build.");
#endif
}

int tpm2_find_device_auto(
                int log_level, /* log level when no device is found */
                char **ret) {
#if HAVE_TPM2
        _cleanup_closedir_ DIR *d = NULL;
        int r;

        r = dlopen_tpm2();
        if (r < 0)
                return log_error_errno(r, "TPM2 support is not installed.");

        d = opendir("/sys/class/tpmrm");
        if (!d) {
                log_full_errno(errno == ENOENT ? LOG_DEBUG : LOG_ERR, errno,
                               "Failed to open /sys/class/tpmrm: %m");
                if (errno != ENOENT)
                        return -errno;
        } else {
                _cleanup_free_ char *node = NULL;

                for (;;) {
                        struct dirent *de;

                        de = readdir_no_dot(d);
                        if (!de)
                                break;

                        if (node)
                                return log_error_errno(SYNTHETIC_ERRNO(ENOTUNIQ),
                                                       "More than one TPM2 (tpmrm) device found.");

                        node = path_join("/dev", de->d_name);
                        if (!node)
                                return log_oom();
                }

                if (node) {
                        *ret = TAKE_PTR(node);
                        return 0;
                }
        }

        return log_full_errno(log_level, SYNTHETIC_ERRNO(ENODEV), "No TPM2 (tpmrm) device found.");
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                               "TPM2 not supported on this build.");
#endif
}

#if HAVE_TPM2
int tpm2_extend_bytes(
                Tpm2Context *c,
                char **banks,
                unsigned pcr_index,
                const void *data,
                size_t data_size,
                const void *secret,
                size_t secret_size) {

#if HAVE_OPENSSL
        TPML_DIGEST_VALUES values = {};
        TSS2_RC rc;

        assert(c);
        assert(data || data_size == 0);
        assert(secret || secret_size == 0);

        if (data_size == SIZE_MAX)
                data_size = strlen(data);
        if (secret_size == SIZE_MAX)
                secret_size = strlen(secret);

        if (pcr_index >= TPM2_PCRS_MAX)
                return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "Can't measure into unsupported PCR %u, refusing.", pcr_index);

        if (strv_isempty(banks))
                return 0;

        STRV_FOREACH(bank, banks) {
                const EVP_MD *implementation;
                int id;

                assert_se(implementation = EVP_get_digestbyname(*bank));

                if (values.count >= ELEMENTSOF(values.digests))
                        return log_error_errno(SYNTHETIC_ERRNO(E2BIG), "Too many banks selected.");

                if ((size_t) EVP_MD_size(implementation) > sizeof(values.digests[values.count].digest))
                        return log_error_errno(SYNTHETIC_ERRNO(E2BIG), "Hash result too large for TPM2.");

                id = tpm2_hash_alg_from_string(EVP_MD_name(implementation));
                if (id < 0)
                        return log_error_errno(id, "Can't map hash name to TPM2.");

                values.digests[values.count].hashAlg = id;

                /* So here's a twist: sometimes we want to measure secrets (e.g. root file system volume
                 * key), but we'd rather not leak a literal hash of the secret to the TPM (given that the
                 * wire is unprotected, and some other subsystem might use the simple, literal hash of the
                 * secret for other purposes, maybe because it needs a shorter secret derived from it for
                 * some unrelated purpose, who knows). Hence we instead measure an HMAC signature of a
                 * private non-secret string instead. */
                if (secret_size > 0) {
                        if (!HMAC(implementation, secret, secret_size, data, data_size, (unsigned char*) &values.digests[values.count].digest, NULL))
                                return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Failed to calculate HMAC of data to measure.");
                } else if (EVP_Digest(data, data_size, (unsigned char*) &values.digests[values.count].digest, NULL, implementation, NULL) != 1)
                        return log_error_errno(SYNTHETIC_ERRNO(ENOTRECOVERABLE), "Failed to hash data to measure.");

                values.count++;
        }

        rc = sym_Esys_PCR_Extend(
                        c->esys_context,
                        ESYS_TR_PCR0 + pcr_index,
                        ESYS_TR_PASSWORD,
                        ESYS_TR_NONE,
                        ESYS_TR_NONE,
                        &values);
        if (rc != TSS2_RC_SUCCESS)
                return log_error_errno(
                                SYNTHETIC_ERRNO(ENOTRECOVERABLE),
                                "Failed to measure into PCR %u: %s",
                                pcr_index,
                                sym_Tss2_RC_Decode(rc));

        return 0;
#else /* HAVE_OPENSSL */
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "OpenSSL support is disabled.");
#endif
}
#endif

char *tpm2_pcr_mask_to_string(uint32_t mask) {
        _cleanup_free_ char *s = NULL;

        FOREACH_PCR_IN_MASK(n, mask)
                if (strextendf_with_separator(&s, "+", "%d", n) < 0)
                        return NULL;

        if (!s)
                return strdup("");

        return TAKE_PTR(s);
}

int tpm2_make_pcr_json_array(uint32_t pcr_mask, JsonVariant **ret) {
        _cleanup_(json_variant_unrefp) JsonVariant *a = NULL;
        int r;

        assert(ret);

        for (size_t i = 0; i < TPM2_PCRS_MAX; i++) {
                _cleanup_(json_variant_unrefp) JsonVariant *e = NULL;

                if ((pcr_mask & (UINT32_C(1) << i)) == 0)
                        continue;

                r = json_variant_new_integer(&e, i);
                if (r < 0)
                        return r;

                r = json_variant_append_array(&a, e);
                if (r < 0)
                        return r;
        }

        if (!a)
                return json_variant_new_array(ret, NULL, 0);

        *ret = TAKE_PTR(a);
        return 0;
}

int tpm2_parse_pcr_json_array(JsonVariant *v, uint32_t *ret) {
        JsonVariant *e;
        uint32_t mask = 0;

        if (!json_variant_is_array(v))
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PCR array is not a JSON array.");

        JSON_VARIANT_ARRAY_FOREACH(e, v) {
                uint64_t u;

                if (!json_variant_is_unsigned(e))
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PCR is not an unsigned integer.");

                u = json_variant_unsigned(e);
                if (u >= TPM2_PCRS_MAX)
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PCR number out of range: %" PRIu64, u);

                mask |= UINT32_C(1) << u;
        }

        if (ret)
                *ret = mask;

        return 0;
}

int tpm2_make_luks2_json(
                int keyslot,
                uint32_t hash_pcr_mask,
                uint16_t pcr_bank,
                const void *pubkey,
                size_t pubkey_size,
                uint32_t pubkey_pcr_mask,
                uint16_t primary_alg,
                const void *blob,
                size_t blob_size,
                const void *policy_hash,
                size_t policy_hash_size,
                const void *salt,
                size_t salt_size,
                const void *srk_buf,
                size_t srk_buf_size,
                TPM2Flags flags,
                JsonVariant **ret) {

        _cleanup_(json_variant_unrefp) JsonVariant *v = NULL, *hmj = NULL, *pkmj = NULL;
        _cleanup_free_ char *keyslot_as_string = NULL;
        int r;

        assert(blob || blob_size == 0);
        assert(policy_hash || policy_hash_size == 0);
        assert(pubkey || pubkey_size == 0);

        if (asprintf(&keyslot_as_string, "%i", keyslot) < 0)
                return -ENOMEM;

        r = tpm2_make_pcr_json_array(hash_pcr_mask, &hmj);
        if (r < 0)
                return r;

        if (pubkey_pcr_mask != 0) {
                r = tpm2_make_pcr_json_array(pubkey_pcr_mask, &pkmj);
                if (r < 0)
                        return r;
        }

        /* Note: We made the mistake of using "-" in the field names, which isn't particular compatible with
         * other programming languages. Let's not make things worse though, i.e. future additions to the JSON
         * object should use "_" rather than "-" in field names. */

        r = json_build(&v,
                       JSON_BUILD_OBJECT(
                                       JSON_BUILD_PAIR("type", JSON_BUILD_CONST_STRING("systemd-tpm2")),
                                       JSON_BUILD_PAIR("keyslots", JSON_BUILD_ARRAY(JSON_BUILD_STRING(keyslot_as_string))),
                                       JSON_BUILD_PAIR("tpm2-blob", JSON_BUILD_BASE64(blob, blob_size)),
                                       JSON_BUILD_PAIR("tpm2-pcrs", JSON_BUILD_VARIANT(hmj)),
                                       JSON_BUILD_PAIR_CONDITION(!!tpm2_hash_alg_to_string(pcr_bank), "tpm2-pcr-bank", JSON_BUILD_STRING(tpm2_hash_alg_to_string(pcr_bank))),
                                       JSON_BUILD_PAIR_CONDITION(!!tpm2_asym_alg_to_string(primary_alg), "tpm2-primary-alg", JSON_BUILD_STRING(tpm2_asym_alg_to_string(primary_alg))),
                                       JSON_BUILD_PAIR("tpm2-policy-hash", JSON_BUILD_HEX(policy_hash, policy_hash_size)),
                                       JSON_BUILD_PAIR("tpm2-pin", JSON_BUILD_BOOLEAN(flags & TPM2_FLAGS_USE_PIN)),
                                       JSON_BUILD_PAIR_CONDITION(pubkey_pcr_mask != 0, "tpm2_pubkey_pcrs", JSON_BUILD_VARIANT(pkmj)),
                                       JSON_BUILD_PAIR_CONDITION(pubkey_pcr_mask != 0, "tpm2_pubkey", JSON_BUILD_BASE64(pubkey, pubkey_size)),
                                       JSON_BUILD_PAIR_CONDITION(salt, "tpm2_salt", JSON_BUILD_BASE64(salt, salt_size)),
                                       JSON_BUILD_PAIR_CONDITION(srk_buf, "tpm2_srk", JSON_BUILD_BASE64(srk_buf, srk_buf_size))));
        if (r < 0)
                return r;

        if (ret)
                *ret = TAKE_PTR(v);

        return keyslot;
}

int tpm2_parse_luks2_json(
                JsonVariant *v,
                int *ret_keyslot,
                uint32_t *ret_hash_pcr_mask,
                uint16_t *ret_pcr_bank,
                void **ret_pubkey,
                size_t *ret_pubkey_size,
                uint32_t *ret_pubkey_pcr_mask,
                uint16_t *ret_primary_alg,
                void **ret_blob,
                size_t *ret_blob_size,
                void **ret_policy_hash,
                size_t *ret_policy_hash_size,
                void **ret_salt,
                size_t *ret_salt_size,
                void **ret_srk_buf,
                size_t *ret_srk_buf_size,
                TPM2Flags *ret_flags) {

        _cleanup_free_ void *blob = NULL, *policy_hash = NULL, *pubkey = NULL, *salt = NULL, *srk_buf = NULL;
        size_t blob_size = 0, policy_hash_size = 0, pubkey_size = 0, salt_size = 0, srk_buf_size = 0;
        uint32_t hash_pcr_mask = 0, pubkey_pcr_mask = 0;
        uint16_t primary_alg = TPM2_ALG_ECC; /* ECC was the only supported algorithm in systemd < 250, use that as implied default, for compatibility */
        uint16_t pcr_bank = UINT16_MAX; /* default: pick automatically */
        int r, keyslot = -1;
        TPM2Flags flags = 0;
        JsonVariant *w;

        assert(v);

        if (ret_keyslot) {
                keyslot = cryptsetup_get_keyslot_from_token(v);
                if (keyslot < 0) {
                        /* Return a recognizable error when parsing this field, so that callers can handle parsing
                         * errors of the keyslots field gracefully, since it's not 'owned' by us, but by the LUKS2
                         * spec */
                        log_debug_errno(keyslot, "Failed to extract keyslot index from TPM2 JSON data token, skipping: %m");
                        return -EUCLEAN;
                }
        }

        w = json_variant_by_key(v, "tpm2-pcrs");
        if (!w)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 token data lacks 'tpm2-pcrs' field.");

        r = tpm2_parse_pcr_json_array(w, &hash_pcr_mask);
        if (r < 0)
                return log_debug_errno(r, "Failed to parse TPM2 PCR mask: %m");

        /* The bank field is optional, since it was added in systemd 250 only. Before the bank was hardcoded
         * to SHA256. */
        w = json_variant_by_key(v, "tpm2-pcr-bank");
        if (w) {
                /* The PCR bank field is optional */

                if (!json_variant_is_string(w))
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PCR bank is not a string.");

                r = tpm2_hash_alg_from_string(json_variant_string(w));
                if (r < 0)
                        return log_debug_errno(r, "TPM2 PCR bank invalid or not supported: %s", json_variant_string(w));

                pcr_bank = r;
        }

        /* The primary key algorithm field is optional, since it was also added in systemd 250 only. Before
         * the algorithm was hardcoded to ECC. */
        w = json_variant_by_key(v, "tpm2-primary-alg");
        if (w) {
                /* The primary key algorithm is optional */

                if (!json_variant_is_string(w))
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 primary key algorithm is not a string.");

                r = tpm2_asym_alg_from_string(json_variant_string(w));
                if (r < 0)
                        return log_debug_errno(r, "TPM2 asymmetric algorithm invalid or not supported: %s", json_variant_string(w));

                primary_alg = r;
        }

        w = json_variant_by_key(v, "tpm2-blob");
        if (!w)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 token data lacks 'tpm2-blob' field.");

        r = json_variant_unbase64(w, &blob, &blob_size);
        if (r < 0)
                return log_debug_errno(r, "Invalid base64 data in 'tpm2-blob' field.");

        w = json_variant_by_key(v, "tpm2-policy-hash");
        if (!w)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 token data lacks 'tpm2-policy-hash' field.");

        r = json_variant_unhex(w, &policy_hash, &policy_hash_size);
        if (r < 0)
                return log_debug_errno(r, "Invalid base64 data in 'tpm2-policy-hash' field.");

        w = json_variant_by_key(v, "tpm2-pin");
        if (w) {
                if (!json_variant_is_boolean(w))
                        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "TPM2 PIN policy is not a boolean.");

                SET_FLAG(flags, TPM2_FLAGS_USE_PIN, json_variant_boolean(w));
        }

        w = json_variant_by_key(v, "tpm2_salt");
        if (w) {
                r = json_variant_unbase64(w, &salt, &salt_size);
                if (r < 0)
                        return log_debug_errno(r, "Invalid base64 data in 'tpm2_salt' field.");
        }

        w = json_variant_by_key(v, "tpm2_pubkey_pcrs");
        if (w) {
                r = tpm2_parse_pcr_json_array(w, &pubkey_pcr_mask);
                if (r < 0)
                        return r;
        }

        w = json_variant_by_key(v, "tpm2_pubkey");
        if (w) {
                r = json_variant_unbase64(w, &pubkey, &pubkey_size);
                if (r < 0)
                        return log_debug_errno(r, "Failed to decode PCR public key.");
        } else if (pubkey_pcr_mask != 0)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Public key PCR mask set, but not public key included in JSON data, refusing.");

        w = json_variant_by_key(v, "tpm2_srk");
        if (w) {
                r = json_variant_unbase64(w, &srk_buf, &srk_buf_size);
                if (r < 0)
                        return log_debug_errno(r, "Invalid base64 data in 'tpm2_srk' field.");
        }

        if (ret_keyslot)
                *ret_keyslot = keyslot;
        if (ret_hash_pcr_mask)
                *ret_hash_pcr_mask = hash_pcr_mask;
        if (ret_pcr_bank)
                *ret_pcr_bank = pcr_bank;
        if (ret_pubkey)
                *ret_pubkey = TAKE_PTR(pubkey);
        if (ret_pubkey_size)
                *ret_pubkey_size = pubkey_size;
        if (ret_pubkey_pcr_mask)
                *ret_pubkey_pcr_mask = pubkey_pcr_mask;
        if (ret_primary_alg)
                *ret_primary_alg = primary_alg;
        if (ret_blob)
                *ret_blob = TAKE_PTR(blob);
        if (ret_blob_size)
                *ret_blob_size = blob_size;
        if (ret_policy_hash)
                *ret_policy_hash = TAKE_PTR(policy_hash);
        if (ret_policy_hash_size)
                *ret_policy_hash_size = policy_hash_size;
        if (ret_salt)
                *ret_salt = TAKE_PTR(salt);
        if (ret_salt_size)
                *ret_salt_size = salt_size;
        if (ret_flags)
                *ret_flags = flags;
        if (ret_srk_buf)
                *ret_srk_buf = TAKE_PTR(srk_buf);
        if (ret_srk_buf_size)
                *ret_srk_buf_size = srk_buf_size;

        return 0;
}

int tpm2_hash_alg_to_size(uint16_t alg) {
        if (alg == TPM2_ALG_SHA1)
                return 20;
        if (alg == TPM2_ALG_SHA256)
                return 32;
        if (alg == TPM2_ALG_SHA384)
                return 48;
        if (alg == TPM2_ALG_SHA512)
                return 64;
        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Unknown hash algorithm id 0x%" PRIx16, alg);
}

const char *tpm2_hash_alg_to_string(uint16_t alg) {
        if (alg == TPM2_ALG_SHA1)
                return "sha1";
        if (alg == TPM2_ALG_SHA256)
                return "sha256";
        if (alg == TPM2_ALG_SHA384)
                return "sha384";
        if (alg == TPM2_ALG_SHA512)
                return "sha512";
        log_debug("Unknown hash algorithm id 0x%" PRIx16, alg);
        return NULL;
}

int tpm2_hash_alg_from_string(const char *alg) {
        if (strcaseeq_ptr(alg, "sha1"))
                return TPM2_ALG_SHA1;
        if (strcaseeq_ptr(alg, "sha256"))
                return TPM2_ALG_SHA256;
        if (strcaseeq_ptr(alg, "sha384"))
                return TPM2_ALG_SHA384;
        if (strcaseeq_ptr(alg, "sha512"))
                return TPM2_ALG_SHA512;
        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Unknown hash algorithm name '%s'", alg);
}

const char *tpm2_asym_alg_to_string(uint16_t alg) {
        if (alg == TPM2_ALG_ECC)
                return "ecc";
        if (alg == TPM2_ALG_RSA)
                return "rsa";
        log_debug("Unknown asymmetric algorithm id 0x%" PRIx16, alg);
        return NULL;
}

int tpm2_asym_alg_from_string(const char *alg) {
        if (strcaseeq_ptr(alg, "ecc"))
                return TPM2_ALG_ECC;
        if (strcaseeq_ptr(alg, "rsa"))
                return TPM2_ALG_RSA;
        return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Unknown asymmetric algorithm name '%s'", alg);
}

Tpm2Support tpm2_support(void) {
        Tpm2Support support = TPM2_SUPPORT_NONE;
        int r;

        if (detect_container() <= 0) {
                /* Check if there's a /dev/tpmrm* device via sysfs. If we run in a container we likely just
                 * got the host sysfs mounted. Since devices are generally not virtualized for containers,
                 * let's assume containers never have a TPM, at least for now. */

                r = dir_is_empty("/sys/class/tpmrm", /* ignore_hidden_or_backup= */ false);
                if (r < 0) {
                        if (r != -ENOENT)
                                log_debug_errno(r, "Unable to test whether /sys/class/tpmrm/ exists and is populated, assuming it is not: %m");
                } else if (r == 0) /* populated! */
                        support |= TPM2_SUPPORT_SUBSYSTEM|TPM2_SUPPORT_DRIVER;
                else
                        /* If the directory exists but is empty, we know the subsystem is enabled but no
                         * driver has been loaded yet. */
                        support |= TPM2_SUPPORT_SUBSYSTEM;
        }

        if (efi_has_tpm2())
                support |= TPM2_SUPPORT_FIRMWARE;

#if HAVE_TPM2
        support |= TPM2_SUPPORT_SYSTEM;

        r = dlopen_tpm2();
        if (r >= 0)
                support |= TPM2_SUPPORT_LIBRARIES;
#endif

        return support;
}

#if HAVE_TPM2
static void tpm2_pcr_values_apply_default_hash_alg(Tpm2PCRValue *pcr_values, size_t n_pcr_values) {
        TPMI_ALG_HASH default_hash = 0;
        for (size_t i = 0; i < n_pcr_values; i++)
                if (pcr_values[i].hash != 0) {
                        default_hash = pcr_values[i].hash;
                        break;
                }

        if (default_hash != 0)
                for (size_t i = 0; i < n_pcr_values; i++)
                        if (pcr_values[i].hash == 0)
                                pcr_values[i].hash = default_hash;
}
#endif

/* Parse the PCR selection/value arg(s) and return a corresponding array of Tpm2PCRValue objects.
 *
 * The format is the same as tpm2_pcr_values_from_string(). The first provided entry with a hash algorithm
 * set will be used as the 'default' hash algorithm. All entries with an unset hash algorithm will be updated
 * with the 'default' hash algorithm. The resulting array will be sorted and checked for validity.
 *
 * This will replace *ret_pcr_values with the new array of pcr values; to append to an existing array, use
 * tpm2_parse_pcr_argument_append(). */
int tpm2_parse_pcr_argument(const char *arg, Tpm2PCRValue **ret_pcr_values, size_t *ret_n_pcr_values) {
#if HAVE_TPM2
        int r;

        assert(arg);
        assert(ret_pcr_values);
        assert(ret_n_pcr_values);

        _cleanup_free_ Tpm2PCRValue *pcr_values = NULL;
        size_t n_pcr_values = 0;
        r = tpm2_pcr_values_from_string(arg, &pcr_values, &n_pcr_values);
        if (r < 0)
                return r;

        tpm2_pcr_values_apply_default_hash_alg(pcr_values, n_pcr_values);

        tpm2_sort_pcr_values(pcr_values, n_pcr_values);

        if (!TPM2_PCR_VALUES_VALID(pcr_values, n_pcr_values))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Parsed PCR values are not valid.");

        *ret_pcr_values = TAKE_PTR(pcr_values);
        *ret_n_pcr_values = n_pcr_values;

        return 0;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "TPM2 support is disabled.");
#endif
}

/* Same as tpm2_parse_pcr_argument(), but the pcr values array is appended to. If the provided pcr values
 * array is not NULL, it must point to an allocated pcr values array and the provided number of pcr values
 * must be correct.
 *
 * Note that 'arg' is parsed into a new array of pcr values independently of any previous pcr values,
 * including application of the default hash algorithm. Then the two arrays are combined, the default hash
 * algorithm check applied again (in case either the previous or current array had no default hash
 * algorithm), and then the resulting array is sorted and rechecked for validity. */
int tpm2_parse_pcr_argument_append(const char *arg, Tpm2PCRValue **ret_pcr_values, size_t *ret_n_pcr_values) {
#if HAVE_TPM2
        int r;

        assert(arg);
        assert(ret_pcr_values);
        assert(ret_n_pcr_values);

        _cleanup_free_ Tpm2PCRValue *pcr_values = NULL;
        size_t n_pcr_values;
        r = tpm2_parse_pcr_argument(arg, &pcr_values, &n_pcr_values);
        if (r < 0)
                return r;

        /* If we got previous values, append them. */
        if (*ret_pcr_values && !GREEDY_REALLOC_APPEND(pcr_values, n_pcr_values, *ret_pcr_values, *ret_n_pcr_values))
                return log_oom();

        tpm2_pcr_values_apply_default_hash_alg(pcr_values, n_pcr_values);

        tpm2_sort_pcr_values(pcr_values, n_pcr_values);

        if (!TPM2_PCR_VALUES_VALID(pcr_values, n_pcr_values))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Parsed PCR values are not valid.");

        SWAP_TWO(*ret_pcr_values, pcr_values);
        *ret_n_pcr_values = n_pcr_values;

        return 0;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "TPM2 support is disabled.");
#endif
}

/* Same as tpm2_parse_pcr_argument() but converts the pcr values to a pcr mask. If more than one hash
 * algorithm is included in the pcr values array this results in error. This retains the previous behavior of
 * tpm2_parse_pcr_argument() of clearing the mask if 'arg' is empty, replacing the mask if it is set to
 * UINT32_MAX, and or-ing the mask otherwise. */
int tpm2_parse_pcr_argument_to_mask(const char *arg, uint32_t *ret_mask) {
#if HAVE_TPM2
        _cleanup_free_ Tpm2PCRValue *pcr_values = NULL;
        size_t n_pcr_values;
        int r;

        assert(arg);
        assert(ret_mask);

        r = tpm2_parse_pcr_argument(arg, &pcr_values, &n_pcr_values);
        if (r < 0)
                return r;

        if (n_pcr_values == 0) {
                /* This retains the previous behavior of clearing the mask if the arg is empty */
                *ret_mask = 0;
                return 0;
        }

        size_t hash_count;
        r = tpm2_pcr_values_hash_count(pcr_values, n_pcr_values, &hash_count);
        if (r < 0)
                return log_error_errno(r, "Could not get hash count from pcr values: %m");

        if (hash_count > 1)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Multiple PCR hash banks selected.");

        uint32_t new_mask;
        r = tpm2_pcr_values_to_mask(pcr_values, n_pcr_values, pcr_values[0].hash, &new_mask);
        if (r < 0)
                return log_error_errno(r, "Could not get pcr values mask: %m");

        if (*ret_mask == UINT32_MAX)
                *ret_mask = new_mask;
        else
                *ret_mask |= new_mask;

        return 0;
#else
        return log_error_errno(SYNTHETIC_ERRNO(EOPNOTSUPP), "TPM2 support is disabled.");
#endif
}

int tpm2_load_pcr_signature(const char *path, JsonVariant **ret) {
        _cleanup_strv_free_ char **search = NULL;
        _cleanup_free_ char *discovered_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        /* Tries to load a JSON PCR signature file. Takes an absolute path, a simple file name or NULL. In
         * the latter two cases searches in /etc/, /usr/lib/, /run/, as usual. */

        search = strv_split_nulstr(CONF_PATHS_NULSTR("systemd"));
        if (!search)
                return log_oom();

        if (!path) {
                /* If no path is specified, then look for "tpm2-pcr-signature.json" automatically. Also, in
                 * this case include /.extra/ in the search path, but only in this case, and if we run in the
                 * initrd. We don't want to be too eager here, after all /.extra/ is untrusted territory. */

                path = "tpm2-pcr-signature.json";

                if (in_initrd())
                        if (strv_extend(&search, "/.extra") < 0)
                                return log_oom();
        }

        r = search_and_fopen(path, "re", NULL, (const char**) search, &f, &discovered_path);
        if (r < 0)
                return log_debug_errno(r, "Failed to find TPM PCR signature file '%s': %m", path);

        r = json_parse_file(f, discovered_path, 0, ret, NULL, NULL);
        if (r < 0)
                return log_debug_errno(r, "Failed to parse TPM PCR signature JSON object '%s': %m", discovered_path);

        return 0;
}

int tpm2_load_pcr_public_key(const char *path, void **ret_pubkey, size_t *ret_pubkey_size) {
        _cleanup_free_ char *discovered_path = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        /* Tries to load a PCR public key file. Takes an absolute path, a simple file name or NULL. In the
         * latter two cases searches in /etc/, /usr/lib/, /run/, as usual. */

        if (!path)
                path = "tpm2-pcr-public-key.pem";

        r = search_and_fopen(path, "re", NULL, (const char**) CONF_PATHS_STRV("systemd"), &f, &discovered_path);
        if (r < 0)
                return log_debug_errno(r, "Failed to find TPM PCR public key file '%s': %m", path);

        r = read_full_stream(f, (char**) ret_pubkey, ret_pubkey_size);
        if (r < 0)
                return log_debug_errno(r, "Failed to load TPM PCR public key PEM file '%s': %m", discovered_path);

        return 0;
}

#define PBKDF2_HMAC_SHA256_ITERATIONS 10000

/*
 * Implements PBKDF2 HMAC SHA256 for a derived keylen of 32
 * bytes and for PBKDF2_HMAC_SHA256_ITERATIONS count.
 * I found the wikipedia entry relevant and it contains links to
 * relevant RFCs:
 *   - https://en.wikipedia.org/wiki/PBKDF2
 *   - https://www.rfc-editor.org/rfc/rfc2898#section-5.2
 */
int tpm2_util_pbkdf2_hmac_sha256(const void *pass,
                    size_t passlen,
                    const void *salt,
                    size_t saltlen,
                    uint8_t ret_key[static SHA256_DIGEST_SIZE]) {

        uint8_t _cleanup_(erase_and_freep) *buffer = NULL;
        uint8_t u[SHA256_DIGEST_SIZE];

        /* To keep this simple, since derived KeyLen (dkLen in docs)
         * Is the same as the hash output, we don't need multiple
         * blocks. Part of the algorithm is to add the block count
         * in, but this can be hardcoded to 1.
         */
        static const uint8_t block_cnt[] = { 0, 0, 0, 1 };

        assert (salt);
        assert (saltlen > 0);
        assert (saltlen <= (SIZE_MAX - sizeof(block_cnt)));
        assert (passlen > 0);

        /*
         * Build a buffer of salt + block_cnt and hmac_sha256 it we
         * do this as we don't have a context builder for HMAC_SHA256.
         */
        buffer = malloc(saltlen + sizeof(block_cnt));
        if (!buffer)
                return -ENOMEM;

        memcpy(buffer, salt, saltlen);
        memcpy(&buffer[saltlen], block_cnt, sizeof(block_cnt));

        hmac_sha256(pass, passlen, buffer, saltlen + sizeof(block_cnt), u);

        /* dk needs to be an unmodified u as u gets modified in the loop */
        memcpy(ret_key, u, SHA256_DIGEST_SIZE);
        uint8_t *dk = ret_key;

        for (size_t i = 1; i < PBKDF2_HMAC_SHA256_ITERATIONS; i++) {
                hmac_sha256(pass, passlen, u, sizeof(u), u);

                for (size_t j=0; j < sizeof(u); j++)
                        dk[j] ^= u[j];
        }

        return 0;
}

static const char* const pcr_index_table[_PCR_INDEX_MAX_DEFINED] = {
        [PCR_PLATFORM_CODE]       = "platform-code",
        [PCR_PLATFORM_CONFIG]     = "platform-config",
        [PCR_EXTERNAL_CODE]       = "external-code",
        [PCR_EXTERNAL_CONFIG]     = "external-config",
        [PCR_BOOT_LOADER_CODE]    = "boot-loader-code",
        [PCR_BOOT_LOADER_CONFIG]  = "boot-loader-config",
        [PCR_HOST_PLATFORM]       = "host-platform",
        [PCR_SECURE_BOOT_POLICY]  = "secure-boot-policy",
        [PCR_KERNEL_INITRD]       = "kernel-initrd",
        [PCR_IMA]                 = "ima",
        [PCR_KERNEL_BOOT]         = "kernel-boot",
        [PCR_KERNEL_CONFIG]       = "kernel-config",
        [PCR_SYSEXTS]             = "sysexts",
        [PCR_SHIM_POLICY]         = "shim-policy",
        [PCR_SYSTEM_IDENTITY]     = "system-identity",
        [PCR_DEBUG]               = "debug",
        [PCR_APPLICATION_SUPPORT] = "application-support",
};

DEFINE_STRING_TABLE_LOOKUP_FROM_STRING_WITH_FALLBACK(pcr_index, int, TPM2_PCRS_MAX - 1);
DEFINE_STRING_TABLE_LOOKUP_TO_STRING(pcr_index, int);
