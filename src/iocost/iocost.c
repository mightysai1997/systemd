/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <unistd.h>

#include "alloc-util.h"
#include "cgroup-util.h"
#include "main-func.h"
#include "path-util.h"
#include "pretty-print.h"
#include "sd-device.h"
#include "sd-hwdb.h"
#include "verbs.h"

#define DEFAULT_SOLUTION "isolatedbandwidth"

static int help(void) {
        _cleanup_free_ char *link = NULL;
        int r;

        r = terminal_urlify_man("systemd-iocost", "1", &link);
        if (r < 0)
                return log_oom();

        printf("%s [OPTIONS...]\n\n"
               "Set up iocost model and qos solutions for known SSDs\n"
               "\nCommands:\n"
               "  apply <path> [solution]    Apply the specified solution to the device represented by <path>\n"
               "                             (solution defaults to "DEFAULT_SOLUTION")\n"
               "  query <path>               Query hwdb and print known solutions for the device represented\n"
               "                             by <path>\n"
               "\nOptions:\n"
               "  -h --help                  Show this help\n"
               "     --version               Show package version\n"
               "\nSee the %s for details.\n",
               program_invocation_short_name,
               link);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "version",   no_argument,       NULL, ARG_VERSION   },
                {}
        };

        int c;

        assert(argc >= 1);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)
                switch (c) {

                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        return 1;
}

enum HwDbParseState {
        MODEL = 1,
        QOS = 2,
        SKIP = 3,
        DONE = 4,
};

static int hwdb_query_for_path(const char *path, sd_hwdb **hwdb, char **modalias) {
        _cleanup_(sd_device_unrefp) sd_device *device = NULL;
        const char *model_name;
        int r;

        r = sd_device_new_from_path(&device, path);
        if (r < 0)
                return log_error_errno(r, "Error looking up device: %m");

        r = sd_hwdb_new(hwdb);
        if (r < 0)
                return log_error_errno(r, "Failed to open hwdb: %m");

        if ((sd_device_get_property_value(device, "ID_MODEL_FROM_DATABASE", &model_name) < 0 &&
             sd_device_get_property_value(device, "ID_MODEL", &model_name) < 0)) {
                log_info("Model name for device %s is unknown", path);
                return 0;
        }

        if (r < 0)
                return log_error_errno(r, "Model name for device %s is unknown", path);

        asprintf(modalias, "block:devname:%s:name:%s", path, model_name);

        return 0;
}

static char *name_from_key(const char *key) {
        _cleanup_strv_free_ char **key_parts = NULL;

        key_parts = strv_split(key, "_");
        return ascii_strlower(strdup(key_parts[2]));
}

static int apply_solution_for_path(const char *path, const char *name_to_apply) {
        _cleanup_(sd_device_unrefp) sd_device *device = NULL;
        _cleanup_free_ char *modalias = NULL;
        _cleanup_(sd_hwdb_unrefp) sd_hwdb *hwdb = NULL;
        enum HwDbParseState state = MODEL;
        _cleanup_free_ char *name = NULL;
        _cleanup_free_ char *qos = NULL;
        _cleanup_free_ char *model = NULL;
        const char *key, *value;
        dev_t dev;
        int r;

        r = sd_device_new_from_path(&device, path);
        if (r < 0)
                return log_error_errno(r, "Error looking up device: %m");

        r = sd_device_get_devnum(device, &dev);
        if (r < 0)
                return log_error_errno(r, "Error getting devnum for device %s: %m", path);

        r = hwdb_query_for_path(path, &hwdb, &modalias);
        if (r < 0)
                return r;

        SD_HWDB_FOREACH_PROPERTY(hwdb, modalias, key, value) {
                if (state == DONE)
                        break;

                switch (state) {
                        case MODEL:
                                name = name_from_key(key);

                                /* Not the parameters we want to apply, skip the QOS line and look for the next. */
                                if (strcmp(name, name_to_apply)) {
                                        state = SKIP;
                                        break;
                                }

                                asprintf(&model, "%u:%u model=linear ctrl=user %s", major(dev), minor(dev), value);

                                state = QOS;
                                break;
                        case QOS:
                                asprintf(&qos, "%u:%u enable=1 ctrl=user %s", major(dev), minor(dev), value);

                                state = DONE;
                                break;
                        case SKIP:
                                state = MODEL;
                                break;
                        case DONE:
                        default:
                                assert_not_reached();
                }
        }

        /* No iocost qos / model parameters found for this device. */
        if (!name)
                return 0;

        if (!model || !qos)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Could not find iocost parameters with name `%s'", name_to_apply);

        printf("Applying `%s' iocost parameters to %s\n\tio.cost.model: %s\n\tio.cost.qos: %s\n", name, path, model, qos);

        r = cg_set_attribute("io", "/", "io.cost.qos", qos);
        if (r < 0)
                return log_error_errno(r, "Failed to set qos: %m");

        r = cg_set_attribute("io", "/", "io.cost.model", model);
        if (r < 0)
                return log_error_errno(r, "Failed to set model: %m");

        return 0;
}

static int show_solutions_for_path(const char *path) {
        _cleanup_free_ char *modalias = NULL;
        _cleanup_(sd_hwdb_unrefp) sd_hwdb *hwdb = NULL;
        enum HwDbParseState state = MODEL;
        const char *key, *value;
        int r;

        r = hwdb_query_for_path(path, &hwdb, &modalias);
        if (r < 0)
                return r;

        printf("Known iocost solutions for %s\n", path);
        SD_HWDB_FOREACH_PROPERTY(hwdb, modalias, key, value) {
                _cleanup_free_ char *name = NULL;

                switch (state) {
                        case MODEL:
                                name = name_from_key(key);
                                printf("\n%s:\n\tio.cost.model: %s\n", name, value);

                                state = QOS;
                                break;
                        case QOS:
                                printf("\tio.cost.qos: %s\n", value);

                                state = MODEL;
                                break;
                        case DONE:
                        default:
                                assert_not_reached();
                }
        }

        return 0;
}

static int validate_path(const char *path) {
        if (!path_startswith(path, "/sys/") && !path_startswith(path, "/dev/")) {
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL),
                                       "Argument is not a /dev or /sys device path.");
        }
        return 0;
}

static int verb_apply(int argc, char *argv[], void *userdata) {
        const char *path;
        const char *name;
        int r;

        path = argv[1];

        if (argc > 2) {
                name = argv[2];
        } else {
                name = DEFAULT_SOLUTION;
        }

        r = validate_path(path);
        if (r < 0) {
                return r;
        }

        return apply_solution_for_path(path, name);
}

static int verb_query(int argc, char *argv[], void *userdata) {
        const char *path;
        int r;

        path = argv[1];

        r = validate_path(path);
        if (r < 0) {
                return r;
        }

        return show_solutions_for_path(path);
}

static int iocost_main(int argc, char *argv[]) {
        static const Verb verbs[] = {
                { "query", 2, 2, 0, verb_query },
                { "apply", 2, 3, 0, verb_apply },
                {},
        };

        return dispatch_verb(argc, argv, verbs, NULL);
}

static int run(int argc, char *argv[]) {
        _cleanup_free_ char *qos = NULL;
        _cleanup_free_ char *model = NULL;
        int r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        return iocost_main(argc, argv);
}

DEFINE_MAIN_FUNCTION(run);
