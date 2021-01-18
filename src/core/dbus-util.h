/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "dissect-image.h"
#include "sd-bus.h"

#include "unit.h"

int bus_property_get_triggered_unit(sd_bus *bus, const char *path, const char *interface, const char *property, sd_bus_message *reply, void *userdata, sd_bus_error *error);

#define BUS_DEFINE_SET_TRANSIENT(function, bus_type, type, cast_type, fmt) \
        int bus_set_transient_##function(                               \
                        Unit *u,                                        \
                        const char *name,                               \
                        cast_type *p,                                   \
                        sd_bus_message *message,                        \
                        UnitWriteFlags flags,                           \
                        sd_bus_error *error) {                          \
                                                                        \
                type v;                                                 \
                int r;                                                  \
                                                                        \
                assert(p);                                              \
                                                                        \
                r = sd_bus_message_read(message, bus_type, &v);         \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {                    \
                        *p = (cast_type) v;                             \
                        unit_write_settingf(u, flags, name,             \
                                            "%s=" fmt, name, v);        \
                }                                                       \
                                                                        \
                return 1;                                               \
        }

#define BUS_DEFINE_SET_TRANSIENT_IS_VALID(function, bus_type, type, cast_type, fmt, check) \
        int bus_set_transient_##function(                               \
                        Unit *u,                                        \
                        const char *name,                               \
                        cast_type *p,                                   \
                        sd_bus_message *message,                        \
                        UnitWriteFlags flags,                           \
                        sd_bus_error *error) {                          \
                                                                        \
                type v;                                                 \
                int r;                                                  \
                                                                        \
                assert(p);                                              \
                                                                        \
                r = sd_bus_message_read(message, bus_type, &v);         \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                if (!check(v))                                          \
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, \
                                                 "Invalid %s setting: " fmt, name, v); \
                                                                        \
                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {                    \
                        *p = (cast_type) v;                             \
                        unit_write_settingf(u, flags, name,             \
                                            "%s=" fmt, name, v);        \
                }                                                       \
                                                                        \
                return 1;                                               \
        }

#define BUS_DEFINE_SET_TRANSIENT_TO_STRING(function, bus_type, type, cast_type, fmt, to_string) \
        int bus_set_transient_##function(                               \
                        Unit *u,                                        \
                        const char *name,                               \
                        cast_type *p,                                   \
                        sd_bus_message *message,                        \
                        UnitWriteFlags flags,                           \
                        sd_bus_error *error) {                          \
                                                                        \
                const char *s;                                          \
                type v;                                                 \
                int r;                                                  \
                                                                        \
                assert(p);                                              \
                                                                        \
                r = sd_bus_message_read(message, bus_type, &v);         \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                s = to_string(v);                                       \
                if (!s)                                                 \
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, \
                                                 "Invalid %s setting: " fmt, name, v); \
                                                                        \
                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {                    \
                        *p = (cast_type) v;                             \
                        unit_write_settingf(u, flags, name,             \
                                            "%s=%s", name, s);          \
                }                                                       \
                                                                        \
                return 1;                                               \
        }

#define BUS_DEFINE_SET_TRANSIENT_TO_STRING_ALLOC(function, bus_type, type, cast_type, fmt, to_string) \
        int bus_set_transient_##function(                               \
                        Unit *u,                                        \
                        const char *name,                               \
                        cast_type *p,                                   \
                        sd_bus_message *message,                        \
                        UnitWriteFlags flags,                           \
                        sd_bus_error *error) {                          \
                                                                        \
                _cleanup_free_ char *s = NULL;                          \
                type v;                                                 \
                int r;                                                  \
                                                                        \
                assert(p);                                              \
                                                                        \
                r = sd_bus_message_read(message, bus_type, &v);         \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                r = to_string(v, &s);                                   \
                if (r == -EINVAL)                                       \
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, \
                                                 "Invalid %s setting: " fmt, name, v); \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {                    \
                        *p = (cast_type) v;                             \
                        unit_write_settingf(u, flags, name,             \
                                            "%s=%s",                    \
                                            name, strempty(s));         \
                }                                                       \
                                                                        \
                return 1;                                               \
        }

#define BUS_DEFINE_SET_TRANSIENT_PARSE(function, type, parse)           \
        int bus_set_transient_##function(                               \
                        Unit *u,                                        \
                        const char *name,                               \
                        type *p,                                        \
                        sd_bus_message *message,                        \
                        UnitWriteFlags flags,                           \
                        sd_bus_error *error) {                          \
                                                                        \
                const char *s;                                          \
                type v;                                                 \
                int r;                                                  \
                                                                        \
                assert(p);                                              \
                                                                        \
                r = sd_bus_message_read(message, "s", &s);              \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                v = parse(s);                                           \
                if (v < 0)                                              \
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, \
                                                 "Invalid %s setting: %s", name, s); \
                                                                        \
                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {                    \
                        *p = v;                                         \
                        unit_write_settingf(u, flags, name,             \
                                            "%s=%s", name, s);          \
                }                                                       \
                                                                        \
                return 1;                                               \
        }

#define BUS_DEFINE_SET_TRANSIENT_PARSE_PTR(function, type, parse)       \
        int bus_set_transient_##function(                               \
                        Unit *u,                                        \
                        const char *name,                               \
                        type *p,                                        \
                        sd_bus_message *message,                        \
                        UnitWriteFlags flags,                           \
                        sd_bus_error *error) {                          \
                                                                        \
                const char *s;                                          \
                type v;                                                 \
                int r;                                                  \
                                                                        \
                assert(p);                                              \
                                                                        \
                r = sd_bus_message_read(message, "s", &s);              \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                r = parse(s, &v);                                       \
                if (r < 0)                                              \
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, \
                                                 "Invalid %s setting: %s", name, s); \
                                                                        \
                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {                    \
                        *p = v;                                         \
                        unit_write_settingf(u, flags, name,             \
                                            "%s=%s", name, strempty(s)); \
                }                                                       \
                                                                        \
                return 1;                                               \
        }

#define BUS_DEFINE_SET_TRANSIENT_STRING_WITH_CHECK(function, check)     \
        int bus_set_transient_##function(                               \
                        Unit *u,                                        \
                        const char *name,                               \
                        char **p,                                       \
                        sd_bus_message *message,                        \
                        UnitWriteFlags flags,                           \
                        sd_bus_error *error) {                          \
                                                                        \
                const char *v;                                          \
                int r;                                                  \
                                                                        \
                assert(p);                                              \
                                                                        \
                r = sd_bus_message_read(message, "s", &v);              \
                if (r < 0)                                              \
                        return r;                                       \
                                                                        \
                if (!isempty(v) && !check(v))                           \
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, \
                                                 "Invalid %s setting: %s", name, v); \
                                                                        \
                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {                    \
                        r = free_and_strdup(p, empty_to_null(v));       \
                        if (r < 0)                                      \
                                return r;                               \
                                                                        \
                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, \
                                            "%s=%s", name, strempty(v)); \
                }                                                       \
                                                                        \
                return 1;                                               \
        }

int bus_set_transient_mode_t(Unit *u, const char *name, mode_t *p, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error);
int bus_set_transient_unsigned(Unit *u, const char *name, unsigned *p, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error);
int bus_set_transient_user_relaxed(Unit *u, const char *name, char **p, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error);
int bus_set_transient_path(Unit *u, const char *name, char **p, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error);
int bus_set_transient_string(Unit *u, const char *name, char **p, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error);
int bus_set_transient_bool(Unit *u, const char *name, bool *p, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error);
int bus_set_transient_percent(Unit *u, const char *name, int *p, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error);
int bus_set_transient_usec_internal(Unit *u, const char *name, usec_t *p, bool fix_0, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error);
static inline int bus_set_transient_usec(Unit *u, const char *name, usec_t *p, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error) {
        return bus_set_transient_usec_internal(u, name, p, false, message, flags, error);
}
static inline int bus_set_transient_usec_fix_0(Unit *u, const char *name, usec_t *p, sd_bus_message *message, UnitWriteFlags flags, sd_bus_error *error) {
        return bus_set_transient_usec_internal(u, name, p, true, message, flags, error);
}
int bus_verify_manage_units_async_full(Unit *u, const char *verb, int capability, const char *polkit_message, bool interactive, sd_bus_message *call, sd_bus_error *error);

int bus_read_mount_options(sd_bus_message *message, sd_bus_error *error, MountOptions **ret_options, char **ret_format_str, const char *separator);
