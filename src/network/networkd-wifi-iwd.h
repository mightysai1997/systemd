/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include "sd-bus.h"

typedef struct Link Link;

int iwd_get_ssid_async(Link *link);
