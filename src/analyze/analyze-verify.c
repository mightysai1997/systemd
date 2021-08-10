/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <stdlib.h>

#include "af-list.h"
#include "alloc-util.h"
#include "all-units.h"
#include "analyze-security.h"
#include "analyze-verify.h"
#include "bus-error.h"
#include "bus-util.h"
#include "log.h"
#include "manager.h"
#include "pager.h"
#include "path-util.h"
#if HAVE_SECCOMP
#  include "seccomp-util.h"
#endif
#include "strv.h"
#include "unit-name.h"
#include "unit-serialize.h"

static int prepare_filename(const char *filename, char **ret) {
        int r;
        const char *name;
        _cleanup_free_ char *abspath = NULL;
        _cleanup_free_ char *dir = NULL;
        _cleanup_free_ char *with_instance = NULL;
        char *c;

        assert(filename);
        assert(ret);

        r = path_make_absolute_cwd(filename, &abspath);
        if (r < 0)
                return r;

        name = basename(abspath);
        if (!unit_name_is_valid(name, UNIT_NAME_ANY))
                return -EINVAL;

        if (unit_name_is_valid(name, UNIT_NAME_TEMPLATE)) {
                r = unit_name_replace_instance(name, "i", &with_instance);
                if (r < 0)
                        return r;
        }

        dir = dirname_malloc(abspath);
        if (!dir)
                return -ENOMEM;

        c = path_join(dir, with_instance ?: name);
        if (!c)
                return -ENOMEM;

        *ret = c;
        return 0;
}

static int generate_path(char **var, char **filenames) {
        const char *old;
        char **filename;

        _cleanup_strv_free_ char **ans = NULL;
        int r;

        STRV_FOREACH(filename, filenames) {
                char *t;

                t = dirname_malloc(*filename);
                if (!t)
                        return -ENOMEM;

                r = strv_consume(&ans, t);
                if (r < 0)
                        return r;
        }

        assert_se(strv_uniq(ans));

        /* First, prepend our directories. Second, if some path was specified, use that, and
         * otherwise use the defaults. Any duplicates will be filtered out in path-lookup.c.
         * Treat explicit empty path to mean that nothing should be appended.
         */
        old = getenv("SYSTEMD_UNIT_PATH");
        if (!streq_ptr(old, "")) {
                if (!old)
                        old = ":";

                r = strv_extend(&ans, old);
                if (r < 0)
                        return r;
        }

        *var = strv_join(ans, ":");
        if (!*var)
                return -ENOMEM;

        return 0;
}

static int verify_socket(Unit *u) {
        Unit *service;
        int r;

        assert(u);

        if (u->type != UNIT_SOCKET)
                return 0;

        r = socket_load_service_unit(SOCKET(u), -1, &service);
        if (r < 0)
                return log_unit_error_errno(u, r, "service unit for the socket cannot be loaded: %m");

        if (service->load_state != UNIT_LOADED)
                return log_unit_error_errno(u, SYNTHETIC_ERRNO(ENOENT),
                                            "service %s not loaded, socket cannot be started.", service->id);

        log_unit_debug(u, "using service unit %s.", service->id);
        return 0;
}

int verify_executable(Unit *u, const ExecCommand *exec, const char *root) {
        int r;

        if (!exec)
                return 0;

        if (exec->flags & EXEC_COMMAND_IGNORE_FAILURE)
                return 0;

        r = find_executable_full(exec->path, root, false, NULL, NULL);
        if (r < 0)
                return log_unit_error_errno(u, r, "Command %s is not executable: %m", exec->path);

        return 0;
}

static int verify_executables(Unit *u, const char *root) {
        ExecCommand *exec;
        int r = 0, k;
        unsigned i;

        assert(u);

        exec =  u->type == UNIT_SOCKET ? SOCKET(u)->control_command :
                u->type == UNIT_MOUNT ? MOUNT(u)->control_command :
                u->type == UNIT_SWAP ? SWAP(u)->control_command : NULL;
        k = verify_executable(u, exec, root);
        if (k < 0 && r == 0)
                r = k;

        if (u->type == UNIT_SERVICE)
                for (i = 0; i < ELEMENTSOF(SERVICE(u)->exec_command); i++) {
                        k = verify_executable(u, SERVICE(u)->exec_command[i], root);
                        if (k < 0 && r == 0)
                                r = k;
                }

        if (u->type == UNIT_SOCKET)
                for (i = 0; i < ELEMENTSOF(SOCKET(u)->exec_command); i++) {
                        k = verify_executable(u, SOCKET(u)->exec_command[i], root);
                        if (k < 0 && r == 0)
                                r = k;
                }

        return r;
}

static int verify_documentation(Unit *u, bool check_man) {
        char **p;
        int r = 0, k;

        STRV_FOREACH(p, u->documentation) {
                log_unit_debug(u, "Found documentation item: %s", *p);

                if (check_man && startswith(*p, "man:")) {
                        k = show_man_page(*p + 4, true);
                        if (k != 0) {
                                if (k < 0)
                                        log_unit_error_errno(u, k, "Can't show %s: %m", *p + 4);
                                else {
                                        log_unit_error(u, "Command 'man %s' failed with code %d", *p + 4, k);
                                        k = -ENOEXEC;
                                }
                                if (r == 0)
                                        r = k;
                        }
                }
        }

        /* Check remote URLs? */

        return r;
}

/* Refactoring SecurityInfo so that it can make use of existing struct variables instead of reading from dbus */
static int helper_security_info(Unit *u, ExecContext *c, CGroupContext *g, SecurityInfo **ret_info) {
        _cleanup_free_ SecurityInfo *info = new0(SecurityInfo, 1);
        if (!info)
                return log_oom();

        assert(ret_info);

        if (u) {
                info->id = u->id;
                info->type = (char *) unit_type_to_string(u->type);
                info->load_state = (char *) unit_load_state_to_string(u->load_state);
                info->fragment_path = u->fragment_path;
                info->default_dependencies = u->default_dependencies;
                info->notify_access = u->type == UNIT_SERVICE ? (char *) notify_access_to_string(SERVICE(u)->notify_access) : NULL;
        }

        if (c) {
                info->ambient_capabilities = c->capability_ambient_set;
                info->capability_bounding_set = c->capability_bounding_set;
                info->user = c->user;
                info->supplementary_groups = c->supplementary_groups;
                info->dynamic_user = c->dynamic_user;
                info->keyring_mode = (char *) exec_keyring_mode_to_string(c->keyring_mode);
                info->protect_proc = (char *) protect_proc_to_string(c->protect_proc);
                info->proc_subset = (char *) proc_subset_to_string(c->proc_subset);
                info->lock_personality = c->lock_personality;
                info->memory_deny_write_execute = c->memory_deny_write_execute;
                info->no_new_privileges = c->no_new_privileges;
                info->protect_hostname = c->protect_hostname;
                info->private_devices = c->private_devices;
                info->private_mounts = c->private_mounts;
                info->private_network = c->private_network;
                info->private_tmp = c->private_tmp;
                info->private_users = c->private_users;
                info->protect_control_groups = c->protect_control_groups;
                info->protect_kernel_modules = c->protect_kernel_modules;
                info->protect_kernel_tunables = c->protect_kernel_tunables;
                info->protect_kernel_logs = c->protect_kernel_logs;
                info->protect_clock = c->protect_clock;
                info->protect_home = (char *) protect_home_to_string(c->protect_home);
                info->protect_system = (char *) protect_system_to_string(c->protect_system);
                info->remove_ipc = c->remove_ipc;
                info->restrict_address_family_inet =
                        info->restrict_address_family_unix =
                        info->restrict_address_family_netlink =
                        info->restrict_address_family_packet =
                        info->restrict_address_family_other =
                        c->address_families_allow_list;

                void *key;
                SET_FOREACH(key, c->address_families) {
                        const char *name;
                        name = af_to_name(PTR_TO_INT(key));
                        if (!name)
                                continue;
                        if (STR_IN_SET(name, "AF_INET", "AF_INET6"))
                                info->restrict_address_family_inet = !c->address_families_allow_list;
                        else if (streq(name, "AF_UNIX"))
                                info->restrict_address_family_unix = !c->address_families_allow_list;
                        else if (streq(name, "AF_NETLINK"))
                                info->restrict_address_family_netlink = !c->address_families_allow_list;
                        else if (streq(name, "AF_PACKET"))
                                info->restrict_address_family_packet = !c->address_families_allow_list;
                        else
                                info->restrict_address_family_other = !c->address_families_allow_list;
                }

                info->restrict_namespaces = c->restrict_namespaces;
                info->restrict_realtime = c->restrict_realtime;
                info->restrict_suid_sgid = c->restrict_suid_sgid;
                info->root_directory = c->root_directory;
                info->root_image = c->root_image;
                info->_umask = c->umask;
                info->system_call_architectures = c->syscall_archs;
                info->system_call_filter_allow_list = c->syscall_allow_list;
                info->system_call_filter = c->syscall_filter;
        }

        if (g) {
                info->delegate = g->delegate;
                info->device_policy = (char *) cgroup_device_policy_to_string(g->device_policy);

                IPAddressAccessItem *i;
                bool deny_ipv4 = false, deny_ipv6 = false;

                LIST_FOREACH(items, i, g->ip_address_deny) {
                        if (i->family == AF_INET && FAMILY_ADDRESS_SIZE(i->family) && i->prefixlen == 0)
                                deny_ipv4 = true;
                        else if (i->family == AF_INET6 && FAMILY_ADDRESS_SIZE(i->family) == 16 && i->prefixlen == 0)
                                deny_ipv6 = true;
                }
                info->ip_address_deny_all = deny_ipv4 && deny_ipv6;

                info->ip_address_allow_localhost = info->ip_address_allow_other = false;
                LIST_FOREACH(items, i, g->ip_address_allow) {
                        if (in_addr_is_localhost(i->family, &i->address))
                                info->ip_address_allow_localhost = true;
                        else
                                info->ip_address_allow_other = true;
                }

                info->ip_filters_custom_ingress = !strv_isempty(g->ip_filters_ingress);
                info->ip_filters_custom_egress = !strv_isempty(g->ip_filters_egress);
                info->device_allow_non_empty = !LIST_IS_EMPTY(g->device_allow);
        }

        *ret_info = TAKE_PTR(info);

        return 0;
}

static int verify_unit(Unit *u, bool check_man, const char *root) {
        _cleanup_(sd_bus_error_free) sd_bus_error err = SD_BUS_ERROR_NULL;
        int r, k;

        assert(u);

        if (DEBUG_LOGGING)
                unit_dump(u, stdout, "\t");

        log_unit_debug(u, "Creating %s/start job", u->id);
        r = manager_add_job(u->manager, JOB_START, u, JOB_REPLACE, NULL, &err, NULL);
        if (r < 0)
                log_unit_error_errno(u, r, "Failed to create %s/start: %s", u->id, bus_error_message(&err, r));

        k = verify_socket(u);
        if (k < 0 && r == 0)
                r = k;

        k = verify_executables(u, root);
        if (k < 0 && r == 0)
                r = k;

        k = verify_documentation(u, check_man);
        if (k < 0 && r == 0)
                r = k;

        return r;
}

int verify_units(char **filenames, UnitFileScope scope, bool check_man, bool run_generators, const char *root) {
        const ManagerTestRunFlags flags =
                MANAGER_TEST_RUN_MINIMAL |
                MANAGER_TEST_RUN_ENV_GENERATORS |
                run_generators * MANAGER_TEST_RUN_GENERATORS;

        _cleanup_(manager_freep) Manager *m = NULL;
        Unit *units[strv_length(filenames)];
        _cleanup_free_ char *var = NULL;
        int r, k, i, count = 0;
        char **filename;

        if (strv_isempty(filenames))
                return 0;

        /* set the path */
        r = generate_path(&var, filenames);
        if (r < 0)
                return log_error_errno(r, "Failed to generate unit load path: %m");

        assert_se(set_unit_path(var) >= 0);

        r = manager_new(scope, flags, &m);
        if (r < 0)
                return log_error_errno(r, "Failed to initialize manager: %m");

        log_debug("Starting manager...");

        r = manager_startup(m, /* serialization= */ NULL, /* fds= */ NULL, root);
        if (r < 0)
                return r;

        manager_clear_jobs(m);

        log_debug("Loading remaining units from the command line...");

        STRV_FOREACH(filename, filenames) {
                _cleanup_free_ char *prepared = NULL;

                log_debug("Handling %s...", *filename);

                k = prepare_filename(*filename, &prepared);
                if (k < 0) {
                        log_error_errno(k, "Failed to prepare filename %s: %m", *filename);
                        if (r == 0)
                                r = k;
                        continue;
                }

                k = manager_load_startable_unit_or_warn(m, NULL, prepared, &units[count]);
                if (k < 0) {
                        if (r == 0)
                                r = k;
                        continue;
                }

                count++;
        }

        for (i = 0; i < count; i++) {
                k = verify_unit(units[i], check_man, root);
                if (k < 0 && r == 0)
                        r = k;
        }

        return r;
}
