/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "sd-journal.h"

int add_filters(sd_journal *j, char **matches);
