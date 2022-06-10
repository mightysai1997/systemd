/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "resolved-manager.h"

int manager_check_resolv_conf(const Manager *m);
int manager_read_resolv_conf(Manager *m);
int manager_write_resolv_conf(Manager *m);
void manager_symlink_stub_to_uplink_resolv_conf(void);

typedef enum ResolvConfMode {
        RESOLV_CONF_UPLINK,
        RESOLV_CONF_STUB,
        RESOLV_CONF_STATIC,
        RESOLV_CONF_FOREIGN,
        RESOLV_CONF_MISSING,
        _RESOLV_CONF_MODE_MAX,
        _RESOLV_CONF_MODE_INVALID = -EINVAL,
} ResolvConfMode;

int resolv_conf_mode(void);
int resolv_conf_start(void);
int resolv_conf_stop(void);

const char* resolv_conf_mode_to_string(ResolvConfMode m) _const_;
ResolvConfMode resolv_conf_mode_from_string(const char *s) _pure_;
