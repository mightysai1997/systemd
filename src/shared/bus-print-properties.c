/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "bus-print-properties.h"
#include "cap-list.h"
#include "cgroup-util.h"
#include "escape.h"
#include "mountpoint-util.h"
#include "nsflags.h"
#include "parse-util.h"
#include "stdio-util.h"
#include "string-util.h"
#include "strv.h"
#include "time-util.h"
#include "user-util.h"

int bus_print_property_value(const char *name, const char *expected_value, bool only_value, const char *value) {
        assert(name);

        if (expected_value && !streq_ptr(expected_value, value))
                return 0;

        if (only_value)
                puts(value);
        else
                printf("%s=%s\n", name, value);

        return 0;
}

int bus_print_property_valuef(const char *name, const char *expected_value, bool only_value, const char *fmt, ...) {
        va_list ap;
        int r;

        assert(name);
        assert(fmt);

        if (expected_value) {
                _cleanup_free_ char *s = NULL;

                va_start(ap, fmt);
                r = vasprintf(&s, fmt, ap);
                va_end(ap);
                if (r < 0)
                        return -ENOMEM;

                if (streq_ptr(expected_value, s)) {
                        if (only_value)
                                puts(s);
                        else
                                printf("%s=%s\n", name, s);
                }

                return 0;
        }

        if (!only_value)
                printf("%s=", name);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        puts("");

        return 0;
}

static int bus_print_property(const char *name, const char *expected_value, sd_bus_message *m, bool value, bool all) {
        char type;
        const char *contents;
        int r;

        assert(name);
        assert(m);

        r = sd_bus_message_peek_type(m, &type, &contents);
        if (r < 0)
                return r;

        switch (type) {

        case SD_BUS_TYPE_STRING: {
                const char *s;

                r = sd_bus_message_read_basic(m, type, &s);
                if (r < 0)
                        return r;

                if (all || !isempty(s)) {
                        bool good;

                        /* This property has a single value, so we need to take
                         * care not to print a new line, everything else is OK. */
                        good = !strchr(s, '\n');
                        bus_print_property_value(name, expected_value, value, good ? s : "[unprintable]");
                }

                return 1;
        }

        case SD_BUS_TYPE_BOOLEAN: {
                int b;

                r = sd_bus_message_read_basic(m, type, &b);
                if (r < 0)
                        return r;

                if (expected_value && parse_boolean(expected_value) != b)
                        return 1;

                bus_print_property_value(name, NULL, value, yes_no(b));
                return 1;
        }

        case SD_BUS_TYPE_UINT64: {
                uint64_t u;

                r = sd_bus_message_read_basic(m, type, &u);
                if (r < 0)
                        return r;

                /* Yes, heuristics! But we can change this check
                 * should it turn out to not be sufficient */

                if (endswith(name, "Timestamp") ||
                    STR_IN_SET(name, "NextElapseUSecRealtime", "LastTriggerUSec", "TimeUSec", "RTCTimeUSec")) {
                        char timestamp[FORMAT_TIMESTAMP_MAX];
                        const char *t;

                        t = format_timestamp(timestamp, sizeof(timestamp), u);
                        if (t || all)
                                bus_print_property_value(name, expected_value, value, strempty(t));

                } else if (strstr(name, "USec")) {
                        char timespan[FORMAT_TIMESPAN_MAX];

                        (void) format_timespan(timespan, sizeof(timespan), u, 0);
                        bus_print_property_value(name, expected_value, value, timespan);

                } else if (streq(name, "CoredumpFilter")) {
                        char buf[STRLEN("0xFFFFFFFF")];

                        xsprintf(buf, "0x%"PRIx64, u);
                        bus_print_property_value(name, expected_value, value, buf);

                } else if (streq(name, "RestrictNamespaces")) {
                        _cleanup_free_ char *s = NULL;
                        const char *result;

                        if ((u & NAMESPACE_FLAGS_ALL) == 0)
                                result = "yes";
                        else if (FLAGS_SET(u, NAMESPACE_FLAGS_ALL))
                                result = "no";
                        else {
                                r = namespace_flags_to_string(u, &s);
                                if (r < 0)
                                        return r;

                                result = strempty(s);
                        }

                        bus_print_property_value(name, expected_value, value, result);

                } else if (streq(name, "MountFlags")) {
                        const char *result;

                        result = mount_propagation_flags_to_string(u);
                        if (!result)
                                return -EINVAL;

                        bus_print_property_value(name, expected_value, value, result);

                } else if (STR_IN_SET(name, "CapabilityBoundingSet", "AmbientCapabilities")) {
                        _cleanup_free_ char *s = NULL;

                        r = capability_set_to_string_alloc(u, &s);
                        if (r < 0)
                                return r;

                        bus_print_property_value(name, expected_value, value, s);

                } else if ((STR_IN_SET(name, "CPUWeight", "StartupCPUWeight", "IOWeight", "StartupIOWeight") && u == CGROUP_WEIGHT_INVALID) ||
                           (STR_IN_SET(name, "CPUShares", "StartupCPUShares") && u == CGROUP_CPU_SHARES_INVALID) ||
                           (STR_IN_SET(name, "BlockIOWeight", "StartupBlockIOWeight") && u == CGROUP_BLKIO_WEIGHT_INVALID) ||
                           (STR_IN_SET(name, "MemoryCurrent", "TasksCurrent") && u == UINT64_MAX) ||
                           (endswith(name, "NSec") && u == UINT64_MAX))

                        bus_print_property_value(name, expected_value, value, "[not set]");

                else if ((STR_IN_SET(name, "DefaultMemoryLow", "DefaultMemoryMin", "MemoryLow", "MemoryHigh", "MemoryMax", "MemorySwapMax", "MemoryLimit") && u == CGROUP_LIMIT_MAX) ||
                         (STR_IN_SET(name, "TasksMax", "DefaultTasksMax") && u == UINT64_MAX) ||
                         (startswith(name, "Limit") && u == UINT64_MAX) ||
                         (startswith(name, "DefaultLimit") && u == UINT64_MAX))

                        bus_print_property_value(name, expected_value, value, "infinity");
                else if (STR_IN_SET(name, "IPIngressBytes", "IPIngressPackets", "IPEgressBytes", "IPEgressPackets") && u == UINT64_MAX)
                        bus_print_property_value(name, expected_value, value, "[no data]");
                else
                        bus_print_property_valuef(name, expected_value, value, "%"PRIu64, u);

                return 1;
        }

        case SD_BUS_TYPE_INT64: {
                int64_t i;

                r = sd_bus_message_read_basic(m, type, &i);
                if (r < 0)
                        return r;

                bus_print_property_valuef(name, expected_value, value, "%"PRIi64, i);
                return 1;
        }

        case SD_BUS_TYPE_UINT32: {
                uint32_t u;

                r = sd_bus_message_read_basic(m, type, &u);
                if (r < 0)
                        return r;

                if (strstr(name, "UMask") || strstr(name, "Mode"))
                        bus_print_property_valuef(name, expected_value, value, "%04o", u);

                else if (streq(name, "UID")) {
                        if (u == UID_INVALID)
                                bus_print_property_value(name, expected_value, value, "[not set]");
                        else
                                bus_print_property_valuef(name, expected_value, value, "%"PRIu32, u);
                } else if (streq(name, "GID")) {
                        if (u == GID_INVALID)
                                bus_print_property_value(name, expected_value, value, "[not set]");
                        else
                                bus_print_property_valuef(name, expected_value, value, "%"PRIu32, u);
                } else
                        bus_print_property_valuef(name, expected_value, value, "%"PRIu32, u);

                return 1;
        }

        case SD_BUS_TYPE_INT32: {
                int32_t i;

                r = sd_bus_message_read_basic(m, type, &i);
                if (r < 0)
                        return r;

                bus_print_property_valuef(name, expected_value, value, "%"PRIi32, i);
                return 1;
        }

        case SD_BUS_TYPE_DOUBLE: {
                double d;

                r = sd_bus_message_read_basic(m, type, &d);
                if (r < 0)
                        return r;

                bus_print_property_valuef(name, expected_value, value, "%g", d);
                return 1;
        }

        case SD_BUS_TYPE_ARRAY:
                if (streq(contents, "s")) {
                        bool first = true;
                        const char *str;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, contents);
                        if (r < 0)
                                return r;

                        while ((r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &str)) > 0) {
                                _cleanup_free_ char *e = NULL;

                                e = shell_maybe_quote(str, 0);
                                if (!e)
                                        return -ENOMEM;

                                if (first) {
                                        if (!value)
                                                printf("%s=", name);
                                        first = false;
                                } else
                                        fputs(" ", stdout);

                                fputs(e, stdout);
                        }
                        if (r < 0)
                                return r;

                        if (first && all && !value)
                                printf("%s=", name);
                        if (!first || all)
                                puts("");

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return r;

                        return 1;

                } else if (streq(contents, "y")) {
                        const uint8_t *u;
                        size_t n;

                        r = sd_bus_message_read_array(m, SD_BUS_TYPE_BYTE, (const void**) &u, &n);
                        if (r < 0)
                                return r;

                        if (all || n > 0) {
                                unsigned i;

                                if (!value)
                                        printf("%s=", name);

                                for (i = 0; i < n; i++)
                                        printf("%02x", u[i]);

                                puts("");
                        }

                        return 1;

                } else if (streq(contents, "u")) {
                        uint32_t *u;
                        size_t n;

                        r = sd_bus_message_read_array(m, SD_BUS_TYPE_UINT32, (const void**) &u, &n);
                        if (r < 0)
                                return r;

                        if (all || n > 0) {
                                unsigned i;

                                if (!value)
                                        printf("%s=", name);

                                for (i = 0; i < n; i++)
                                        printf("%08x", u[i]);

                                puts("");
                        }

                        return 1;
                }

                break;
        }

        return 0;
}

int bus_message_print_all_properties(
                sd_bus_message *m,
                bus_message_print_t func,
                char **filter,
                bool value,
                bool all,
                Set **found_properties) {

        int r;

        assert(m);

        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r < 0)
                return r;

        while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                _cleanup_free_ char *name_with_equal = NULL;
                const char *name, *contents, *expected_value = NULL;

                r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &name);
                if (r < 0)
                        return r;

                if (found_properties) {
                        r = set_ensure_put(found_properties, &string_hash_ops, name);
                        if (r < 0)
                                return log_oom();
                }

                name_with_equal = strjoin(name, "=");
                if (!name_with_equal)
                        return log_oom();

                if (!filter || strv_find(filter, name) ||
                    (expected_value = strv_find_startswith(filter, name_with_equal))) {
                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0)
                                return r;

                        if (func)
                                r = func(name, expected_value, m, value, all);
                        if (!func || r == 0)
                                r = bus_print_property(name, expected_value, m, value, all);
                        if (r < 0)
                                return r;
                        if (r == 0) {
                                if (all && !expected_value)
                                        printf("%s=[unprintable]\n", name);
                                /* skip what we didn't read */
                                r = sd_bus_message_skip(m, contents);
                                if (r < 0)
                                        return r;
                        }

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return r;
                } else {
                        r = sd_bus_message_skip(m, "v");
                        if (r < 0)
                                return r;
                }

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                        return r;
        }
        if (r < 0)
                return r;

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        return 0;
}

int bus_print_all_properties(
                sd_bus *bus,
                const char *dest,
                const char *path,
                bus_message_print_t func,
                char **filter,
                bool value,
                bool all,
                Set **found_properties) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        assert(bus);
        assert(path);

        r = sd_bus_call_method(bus,
                        dest,
                        path,
                        "org.freedesktop.DBus.Properties",
                        "GetAll",
                        &error,
                        &reply,
                        "s", "");
        if (r < 0)
                return r;

        return bus_message_print_all_properties(reply, func, filter, value, all, found_properties);
}
