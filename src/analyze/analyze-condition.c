/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdlib.h>

#include "analyze-condition.h"
#include "condition.h"
#include "conf-parser.h"
#include "load-fragment.h"
#include "service.h"

static int parse_condition(Unit *u, const char *line) {
        assert(u);
        assert(line);

        for (ConditionType t = 0; t < _CONDITION_TYPE_MAX; t++) {
                ConfigParserCallback callback;
                Condition **target;
                const char *p, *name;

                name = condition_type_to_string(t);
                p = startswith(line, name);
                if (p)
                        target = &u->conditions;
                else {
                        name = assert_type_to_string(t);
                        p = startswith(line, name);
                        if (!p)
                                continue;

                        target = &u->asserts;
                }

                p += strspn(p, WHITESPACE);

                if (*p != '=')
                        continue;
                p++;

                p += strspn(p, WHITESPACE);

                if (condition_takes_path(t))
                        callback = config_parse_unit_condition_path;
                else
                        callback = config_parse_unit_condition_string;

                return callback(NULL, "(cmdline)", 0, NULL, 0, name, t, p, target, u);
        }

        return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "Cannot parse \"%s\".", line);
}

_printf_(7, 8)
static int log_helper(void *userdata, int level, int error, const char *file, int line, const char *func, const char *format, ...) {
        Unit *u = userdata;
        va_list ap;
        int r;

        assert(u);

        /* "upgrade" debug messages */
        level = MIN(LOG_INFO, level);

        va_start(ap, format);
        r = log_object_internalv(level, error, file, line, func,
                                 NULL,
                                 u->id,
                                 NULL,
                                 NULL,
                                 format, ap);
        va_end(ap);

        return r;
}

int verify_conditions(char **lines, UnitFileScope scope) {
        _cleanup_(manager_freep) Manager *m = NULL;
        Unit *u;
        char **line;
        int r, q = 1;

        r = manager_new(scope, MANAGER_TEST_RUN_MINIMAL, &m);
        if (r < 0)
                return log_error_errno(r, "Failed to initialize manager: %m");

        log_debug("Starting manager...");
        r = manager_startup(m, /* serialization= */ NULL, /* fds= */ NULL, /* root= */ NULL);
        if (r < 0)
                return r;

        r = unit_new_for_name(m, sizeof(Service), "test.service", &u);
        if (r < 0)
                return log_error_errno(r, "Failed to create test.service: %m");

        STRV_FOREACH(line, lines) {
                r = parse_condition(u, *line);
                if (r < 0)
                        return r;
        }

        r = condition_test_list(u->asserts, environ, assert_type_to_string, log_helper, u);
        if (u->asserts)
                log_notice("Asserts %s.", r > 0 ? "succeeded" : "failed");

        q = condition_test_list(u->conditions, environ, condition_type_to_string, log_helper, u);
        if (u->conditions)
                log_notice("Conditions %s.", q > 0 ? "succeeded" : "failed");

        return r > 0 && q > 0 ? 0 : -EIO;
}
