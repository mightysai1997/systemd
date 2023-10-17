/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <sys/types.h>

typedef struct MachineCredential {
        char *id;
        void *data;
        size_t size;
} MachineCredential;

void machine_credential_free_all(MachineCredential *creds, size_t n);
int machine_credential_set(MachineCredential **arg_credentials, size_t *arg_n_credentials, const char *cred_string);
int machine_credential_load(MachineCredential **arg_credentials, size_t *arg_n_credentials, const char *cred_path);
