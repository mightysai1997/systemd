/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright © 2020 VMware, Inc. */
#pragma once

#include "netif-sriov.h"

typedef struct Link Link;

int link_request_sr_iov_vfs(Link *link);
bool sr_iov_vf_is_ready_to_configure(Link *link);
