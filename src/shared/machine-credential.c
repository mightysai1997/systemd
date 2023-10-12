/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "creds-util.h"
#include "escape.h"
#include "extract-word.h"
#include "fileio.h"
#include "macro.h"
#include "memory-util.h"
#include "machine-credential.h"
#include "path-util.h"
#include "string-util-fundamental.h"

static void machine_credential_free(MachineCredential *cred) {
        assert(cred);

        cred->id = mfree(cred->id);
        cred->data = erase_and_free(cred->data);
        cred->size = 0;
}

void machine_credential_free_all(MachineCredential *creds, size_t n) {
        size_t i;

        assert(creds || n == 0);

        for (i = 0; i < n; i++)
                machine_credential_free(creds + i);

        free(creds);
}

int machine_credential_set(MachineCredential **arg_credentials, size_t *arg_n_credentials, const char *cred_string) {
        _cleanup_free_ char *word = NULL, *data = NULL;
        MachineCredential *creds = *arg_credentials;
        ssize_t l;
        size_t n_creds = *arg_n_credentials;
        int r;
        const char *p = cred_string;

        r = extract_first_word(&p, &word, ":", EXTRACT_DONT_COALESCE_SEPARATORS);
        if (r == -ENOMEM)
                return r;
        if (r < 0)
                return log_debug_errno(r, "Failed to parse --set-credential= parameter: %m");
        if (r == 0 || !p)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Missing value for --set-credential=: %s", cred_string);

        if (!credential_name_valid(word))
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "MachineCredential name is not valid: %s", word);

        for (size_t i = 0; i < n_creds; i++)
                if (streq(creds[i].id, word))
                        return log_debug_errno(SYNTHETIC_ERRNO(EEXIST), "Duplicate credential '%s', refusing.", word);

        l = cunescape(p, UNESCAPE_ACCEPT_NUL, &data);
        if (l < 0)
                return log_debug_errno(l, "Failed to unescape credential data: %s", p);

        creds = GREEDY_REALLOC(creds, n_creds + 1);
        if (!creds)
                return -ENOMEM;

        creds[n_creds++] = (MachineCredential) {
                .id = TAKE_PTR(word),
                .data = TAKE_PTR(data),
                .size = l,
        };

        *arg_credentials = creds;
        *arg_n_credentials = n_creds;

        return 0;
}

int machine_credential_load(MachineCredential **arg_credentials, size_t *arg_n_credentials, const char *cred_path) {
        ReadFullFileFlags flags = READ_FULL_FILE_SECURE;
        _cleanup_(erase_and_freep) char *data = NULL;
        _cleanup_free_ char *word = NULL, *j = NULL;
        MachineCredential *creds = *arg_credentials;
        size_t size, i, n_creds = *arg_n_credentials;
        int r;
        const char *p = cred_path;

        r = extract_first_word(&p, &word, ":", EXTRACT_DONT_COALESCE_SEPARATORS);
        if (r == -ENOMEM)
                return -ENOMEM;
        if (r < 0)
                return log_debug_errno(r, "Failed to parse --load-credential= parameter: %m");
        if (r == 0 || !p)
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "Missing value for --load-credential=: %s", optarg);

        if (!credential_name_valid(word))
                return log_debug_errno(SYNTHETIC_ERRNO(EINVAL), "MachineCredential name is not valid: %s", word);

        for (i = 0; i < n_creds; i++)
                if (streq(creds[i].id, word))
                        return log_debug_errno(SYNTHETIC_ERRNO(EEXIST), "Duplicate credential '%s', refusing.", word);

        if (path_is_absolute(p))
                flags |= READ_FULL_FILE_CONNECT_SOCKET;
        else {
                const char *e;

                r = get_credentials_dir(&e);
                if (r < 0)
                        return log_debug_errno(r, "MachineCredential not available (no credentials passed at all): %s", word);

                j = path_join(e, p);
                if (!j)
                        return -ENOMEM;
        }

        r = read_full_file_full(AT_FDCWD, j ?: p, UINT64_MAX, SIZE_MAX,
                                flags,
                                NULL,
                                &data, &size);
        if (r < 0)
                return log_debug_errno(r, "Failed to read credential '%s': %m", j ?: p);

        creds = GREEDY_REALLOC(creds, n_creds + 1);
        if (!creds)
                return -ENOMEM;

        creds[n_creds++] = (MachineCredential) {
                .id = TAKE_PTR(word),
                .data = TAKE_PTR(data),
                .size = size,
        };

        *arg_credentials = creds;
        *arg_n_credentials = n_creds;

        return 0;
}
