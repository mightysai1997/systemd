/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "journald-context.h"

int client_context_check_keep_log(ClientContext *c, const char *message);
