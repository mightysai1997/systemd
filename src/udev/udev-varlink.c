/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "udev-manager.h"
#include "udev-varlink.h"
#include "varlink-io.systemd.service.h"
#include "varlink-io.systemd.udev.h"

static int vl_method_reload(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {
        Manager *m = ASSERT_PTR(userdata);

        assert(link);
        assert(parameters);

        if (json_variant_elements(parameters) > 0)
                return varlink_error_invalid_parameter(link, parameters);

        log_debug("Received io.systemd.service.Reload()");

        manager_reload(m, /* force = */ true);

        return varlink_reply(link, NULL);
}

static int vl_method_set_log_level(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {
        static const JsonDispatch dispatch_table[] = {
                {"level", JSON_VARIANT_INTEGER, json_dispatch_int64, 0, JSON_MANDATORY},
                {}
        };

        Manager *m = ASSERT_PTR(userdata);
        int64_t level;
        int r;

        assert(link);
        assert(parameters);

        r = varlink_dispatch(link, parameters, dispatch_table, &level);
        if (r < 0)
                return r;

        if (LOG_PRI(level) != level)
                return varlink_error_invalid_parameter(link, parameters);

        log_debug("Received io.systemd.system.SetLogLevel(%" PRIi64 ")", level);

        manager_set_log_level(m, level);

        return varlink_reply(link, NULL);
}

static int update_exec_queue(Varlink *link, JsonVariant *parameters, void *userdata, bool stop) {
        Manager *m = ASSERT_PTR(userdata);

        assert(link);

        if (json_variant_elements(parameters) > 0)
                return varlink_error_invalid_parameter(link, parameters);

        log_debug("Received io.systemd.udev.%sExecQueue()", stop ? "Stop" : "Start");

        m->stop_exec_queue = stop;

        return varlink_reply(link, NULL);
}

static int vl_method_stop_exec_queue(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {
        return update_exec_queue(link, parameters, userdata, /* stop = */ true);
}

static int vl_method_start_exec_queue(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {
        return update_exec_queue(link, parameters, userdata, /* stop = */ false);
}

static int vl_method_set_environment(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {
        static const JsonDispatch dispatch_table[] = {
                {"assignments", JSON_VARIANT_ARRAY, json_dispatch_strv, 0, JSON_MANDATORY},
                {}
        };

        Manager *m = ASSERT_PTR(userdata);
        _cleanup_strv_free_ char **assignments = NULL;
        int r;

        assert(link);
        assert(parameters);

        r = varlink_dispatch(link, parameters, dispatch_table, &assignments);
        if (r < 0)
                return r;

        log_debug("Received io.systemd.udev.SetEnvironment()");

        r = manager_set_environment(m, assignments);
        if (r < 0)
                return r;

        return varlink_reply(link, NULL);
}

static int vl_method_unset_environment(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {
        static const JsonDispatch dispatch_table[] = {
                {"names", JSON_VARIANT_ARRAY, json_dispatch_strv, 0, JSON_MANDATORY},
                {}
        };

        Manager *m = ASSERT_PTR(userdata);
        _cleanup_strv_free_ char **names = NULL;
        int r;

        assert(link);
        assert(parameters);

        r = varlink_dispatch(link, parameters, dispatch_table, &names);
        if (r < 0)
                return r;

        log_debug("Received io.systemd.udev.UnsetEnvironment()");

        r = manager_unset_environment(m, names);
        if (r < 0)
                return r;

        return varlink_reply(link, NULL);
}

static int vl_method_set_children_max(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {
        static const JsonDispatch dispatch_table[] = {
                {"n", JSON_VARIANT_UNSIGNED, json_dispatch_uint64, 0, JSON_MANDATORY},
                {}
        };

        Manager *m = ASSERT_PTR(userdata);
        uint64_t n;
        int r;

        assert(link);
        assert(parameters);

        r = varlink_dispatch(link, parameters, dispatch_table, &n);
        if (r < 0)
                return r;

        log_debug("Received io.systemd.udev.SetChildrenMax(%" PRIu64 ")", n);

        manager_set_children_max(m, n);

        return varlink_reply(link, NULL);
}

static int vl_method_exit(Varlink *link, JsonVariant *parameters, VarlinkMethodFlags flags, void *userdata) {
        Manager *m = ASSERT_PTR(userdata);

        assert(link);

        if (json_variant_elements(parameters) > 0)
                return varlink_error_invalid_parameter(link, parameters);

        log_debug("Received io.systemd.udev.Exit()");

        manager_exit(m);

        return varlink_reply(link, NULL);
}

int udev_varlink_connect(Varlink **ret) {
        _cleanup_(varlink_flush_close_unrefp) Varlink *link = NULL;
        int r;

        assert(ret);

        r = varlink_connect_address(&link, UDEV_VARLINK_ADDRESS);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to " UDEV_VARLINK_ADDRESS ": %m");

        (void) varlink_set_description(link, "udev");
        (void) varlink_set_relative_timeout(link, USEC_INFINITY);

        *ret = TAKE_PTR(link);

        return 0;
}

int udev_varlink_call(Varlink *link, const char *method, JsonVariant *parameters, JsonVariant **ret_parameters) {
        const char *error;
        int r;

        assert(link);
        assert(method);

        r = varlink_call(link, method, parameters, ret_parameters, &error, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to execute varlink call: %m");
        if (error)
                return log_error_errno(SYNTHETIC_ERRNO(EBADE),
                                       "Failed to execute varlink call: %s", error);

        return 0;
}

int manager_open_varlink(Manager *m, int fd) {
        int r;

        assert(m);
        assert(m->event);
        assert(!m->varlink_server);

        r = varlink_server_new(&m->varlink_server, VARLINK_SERVER_ROOT_ONLY|VARLINK_SERVER_INHERIT_USERDATA);
        if (r < 0)
                return r;

        varlink_server_set_userdata(m->varlink_server, m);

        r = varlink_server_add_interface_many(
                        m->varlink_server,
                        &vl_interface_io_systemd_service,
                        &vl_interface_io_systemd_udev);
        if (r < 0)
                return r;

        r = varlink_server_bind_method_many(
                        m->varlink_server,
                        "io.systemd.service.Ping", varlink_method_ping,
                        "io.systemd.service.Reload", vl_method_reload,
                        "io.systemd.service.SetLogLevel", vl_method_set_log_level,

                        "io.systemd.udev.Exit", vl_method_exit,
                        "io.systemd.udev.SetChildrenMax", vl_method_set_children_max,
                        "io.systemd.udev.SetEnvironment", vl_method_set_environment,
                        "io.systemd.udev.UnsetEnvironment", vl_method_unset_environment,
                        "io.systemd.udev.StartExecQueue", vl_method_start_exec_queue,
                        "io.systemd.udev.StopExecQueue",  vl_method_stop_exec_queue);
        if (r < 0)
                return r;

        r = fd < 0 ? varlink_server_listen_address(m->varlink_server, UDEV_VARLINK_ADDRESS, 0600)
                   : varlink_server_listen_fd(m->varlink_server, fd);
        if (r < 0)
                return r;

        r = varlink_server_attach_event(m->varlink_server, m->event, SD_EVENT_PRIORITY_NORMAL);
        if (r < 0)
                return r;

        return 0;
}
