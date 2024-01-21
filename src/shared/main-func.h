/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdlib.h>

#include "sd-daemon.h"

#include "argv-util.h"
#include "hashmap.h"
#include "pager.h"
#include "selinux-util.h"
#include "spawn-ask-password-agent.h"
#include "spawn-polkit-agent.h"
#include "static-destruct.h"

#define _DEFINE_MAIN_FUNCTION(intro, impl, result_to_exit_status)       \
        int main(int argc, char *argv[]) {                              \
                int r;                                                  \
                assert_se(argc > 0 && !isempty(argv[0]));               \
                save_argc_argv(argc, argv);                             \
                intro;                                                  \
                r = impl;                                               \
                if (r < 0)                                              \
                        (void) sd_notifyf(0, "ERRNO=%i", -r);           \
                (void) sd_notifyf(0, "EXIT_STATUS=%i",                  \
                                  result_to_exit_status(r));            \
                ask_password_agent_close();                             \
                polkit_agent_close();                                   \
                pager_close();                                          \
                mac_selinux_finish();                                   \
                static_destruct();                                      \
                hashmap_trim_pools();                                   \
                return result_to_exit_status(r);                        \
        }

static inline int fail_on_negative(int result) {
        return result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Negative return values from impl are mapped to EXIT_FAILURE, and
 * everything else means success! */
#define DEFINE_MAIN_FUNCTION(impl)                                      \
        _DEFINE_MAIN_FUNCTION(,impl(argc, argv), fail_on_negative)

static inline int fail_on_nonzero(int result) {
        return result < 0 ? EXIT_FAILURE : result;
}

/* Zero is mapped to EXIT_SUCCESS, negative values are mapped to EXIT_FAILURE,
 * and positive values are propagated.
 * Note: "true" means failure! */
#define DEFINE_MAIN_FUNCTION_WITH_POSITIVE_FAILURE(impl)                \
        _DEFINE_MAIN_FUNCTION(,impl(argc, argv), fail_on_nonzero)
