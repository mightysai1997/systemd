/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include "alloc-util.h"
#include "macro.h"
#include "memory-util.h"

/* A framework for registering static variables that shall be freed on shutdown of a process. It's a bit like gcc's
 * destructor attribute, but allows us to precisely schedule when we want to free the variables. This is supposed to
 * feel a bit like the gcc cleanup attribute, but for static variables. Note that this does not work for static
 * variables declared in .so's, as the list is private to the same linking unit. But maybe that's a good thing. */

#define _common_static_destruct_attrs_                                  \
        /* Older compilers don't know "retain" attribute. */            \
        _Pragma("GCC diagnostic ignored \"-Wattributes\"")              \
        /* Use pointer alignment, since that is apparently what gcc does for static variables. */ \
        _alignptr_                                                      \
        /* Make sure this is not dropped from the image despite not being explicitly referenced. */ \
        _used_                                                          \
        /* Prevent garbage collection by the linker. */                 \
        _retain_                                                        \
        /* Make sure that AddressSanitizer doesn't pad this variable: we want everything in this section
         * packed next to each other so that we can enumerate it. */    \
        _variable_no_sanitize_address_

typedef struct StaticDestructor {
        void *data;
        free_func_t destroy;
} StaticDestructor;

#define STATIC_DESTRUCTOR_REGISTER(variable, func) \
        _STATIC_DESTRUCTOR_REGISTER(UNIQ, variable, func)

#define _STATIC_DESTRUCTOR_REGISTER(uq, variable, func)                 \
        /* Type-safe destructor */                                      \
        static void UNIQ_T(static_destructor_wrapper, uq)(void *p) {    \
                typeof(variable) *q = p;                                \
                func(q);                                                \
        }                                                               \
        _common_static_destruct_attrs_                                  \
        /* The actual destructor structure we place in a special section to find it. */ \
        _section_("SYSTEMD_STATIC_DESTRUCT")                            \
        static const StaticDestructor UNIQ_T(static_destructor_entry, uq) = { \
                .data = &(variable),                                    \
                .destroy = UNIQ_T(static_destructor_wrapper, uq),       \
        }

#define STATIC_ARRAY_DESTRUCTOR_REGISTER(array, n, func)        \
        _STATIC_ARRAY_DESTRUCTOR_REGISTER(UNIQ, array, n, func)

#define _STATIC_ARRAY_DESTRUCTOR_REGISTER(uq, array, n, func)           \
        _unused_ static void (* UNIQ_T(static_arrray_destructor_wrapper, uq))(typeof(array[0]) *a, size_t b) = func; \
        _common_static_destruct_attrs_                                  \
        /* The actual destructor structure we place in a special section to find it. */ \
        _section_("SYSTEMD_STATIC_ARRAY_DESTRUCT")                      \
        static const ArrayCleanup UNIQ_T(_cleanup_array_, uq) = {       \
                .parray = (void**) &(array),                            \
                .pn = &(n),                                             \
                .pfunc = (free_array_func_t) func,                      \
        };

/* Beginning and end of our section listing the destructors. We define these as weak as we want this to work
 * even if no destructors are defined and the section is missing. */
extern const StaticDestructor _weak_ __start_SYSTEMD_STATIC_DESTRUCT[];
extern const StaticDestructor _weak_ __stop_SYSTEMD_STATIC_DESTRUCT[];
extern const ArrayCleanup _weak_ __start_SYSTEMD_STATIC_ARRAY_DESTRUCT[];
extern const ArrayCleanup _weak_ __stop_SYSTEMD_STATIC_ARRAY_DESTRUCT[];

/* The function to destroy everything. (Note that this must be static inline, as it's key that it remains in
 * the same linking unit as the variables we want to destroy.) */
static inline void static_destruct(void) {
        if (__start_SYSTEMD_STATIC_DESTRUCT)
                for (const StaticDestructor *d = ALIGN_PTR(__start_SYSTEMD_STATIC_DESTRUCT);
                     d < __stop_SYSTEMD_STATIC_DESTRUCT;
                     d = ALIGN_PTR(d + 1))
                        d->destroy(d->data);

        if (__start_SYSTEMD_STATIC_ARRAY_DESTRUCT)
                for (const ArrayCleanup *d = ALIGN_PTR(__start_SYSTEMD_STATIC_ARRAY_DESTRUCT);
                     d < __stop_SYSTEMD_STATIC_ARRAY_DESTRUCT;
                     d = ALIGN_PTR(d + 1))
                        array_cleanup(d);
}
