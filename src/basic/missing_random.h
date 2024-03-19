/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "macro.h"

#if USE_SYS_RANDOM_H
#  include <sys/random.h>
#else
#  include <linux/random.h>
#endif

#ifndef GRND_NONBLOCK
#  define GRND_NONBLOCK 0x0001
#else
static_assert(GRND_NONBLOCK == 0x0001);
#endif

#ifndef GRND_RANDOM
#  define GRND_RANDOM 0x0002
#else
static_assert(GRND_RANDOM == 0x0002);
#endif

#ifndef GRND_INSECURE
#  define GRND_INSECURE 0x0004
#else
static_assert(GRND_INSECURE == 0x0004);
#endif
