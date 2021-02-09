/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stdio.h>

typedef enum HostnameSource {
        HOSTNAME_STATIC,     /* from /etc/hostname */
        HOSTNAME_TRANSIENT,  /* a transient hostname set through systemd, hostnamed, the container manager, or otherwise */
        HOSTNAME_FALLBACK,   /* the compiled-in fallback was used */
        _HOSTNAME_INVALID = -EINVAL,
} HostnameSource;

const char* hostname_source_to_string(HostnameSource source) _const_;
HostnameSource hostname_source_from_string(const char *str) _pure_;

int sethostname_idempotent(const char *s);

int shorten_overlong(const char *s, char **ret);

int read_etc_hostname_stream(FILE *f, char **ret);
int read_etc_hostname(const char *path, char **ret);

bool get_hostname_filtered(char ret[static HOST_NAME_MAX + 1]);
void hostname_update_source_hint(const char *hostname, HostnameSource source);
int hostname_setup(bool really);
