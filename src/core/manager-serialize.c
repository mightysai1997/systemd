/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "clean-ipc.h"
#include "core-varlink.h"
#include "dbus.h"
#include "fd-util.h"
#include "fileio.h"
#include "format-util.h"
#include "initrd-util.h"
#include "macro.h"
#include "manager-serialize.h"
#include "manager.h"
#include "parse-util.h"
#include "serialize.h"
#include "syslog-util.h"
#include "unit-serialize.h"
#include "user-util.h"
#include "varlink-internal.h"

int manager_open_serialization(Manager *m, FILE **ret_f) {
        assert(ret_f);

        return open_serialization_file("systemd-state", ret_f);
}

static bool manager_timestamp_shall_serialize(ManagerTimestamp t) {
        if (!in_initrd())
                return true;

        /* The following timestamps only apply to the host system, hence only serialize them there */
        return !IN_SET(t,
                       MANAGER_TIMESTAMP_USERSPACE, MANAGER_TIMESTAMP_FINISH,
                       MANAGER_TIMESTAMP_SECURITY_START, MANAGER_TIMESTAMP_SECURITY_FINISH,
                       MANAGER_TIMESTAMP_GENERATORS_START, MANAGER_TIMESTAMP_GENERATORS_FINISH,
                       MANAGER_TIMESTAMP_UNITS_LOAD_START, MANAGER_TIMESTAMP_UNITS_LOAD_FINISH);
}

static void manager_serialize_uid_refs_internal(
                FILE *f,
                Hashmap *uid_refs,
                const char *field_name) {

        void *p, *k;

        assert(f);
        assert(field_name);

        /* Serialize the UID reference table. Or actually, just the IPC destruction flag of it, as
         * the actual counter of it is better rebuild after a reload/reexec. */

        HASHMAP_FOREACH_KEY(p, k, uid_refs) {
                uint32_t c;
                uid_t uid;

                uid = PTR_TO_UID(k);
                c = PTR_TO_UINT32(p);

                if (!(c & DESTROY_IPC_FLAG))
                        continue;

                (void) serialize_item_format(f, field_name, UID_FMT, uid);
        }
}

static void manager_serialize_uid_refs(Manager *m, FILE *f) {
        manager_serialize_uid_refs_internal(f, m->uid_refs, "destroy-ipc-uid");
}

static void manager_serialize_gid_refs(Manager *m, FILE *f) {
        manager_serialize_uid_refs_internal(f, m->gid_refs, "destroy-ipc-gid");
}

int manager_serialize(
                Manager *m,
                FILE *f,
                FDSet *fds,
                bool switching_root) {

        const char *t;
        Unit *u;
        int r;

        assert(m);
        assert(f);
        assert(fds);

        _cleanup_(manager_reloading_stopp) _unused_ Manager *reloading = manager_reloading_start(m);

        (void) serialize_item_format(f, "current-job-id", "%" PRIu32, m->current_job_id);
        (void) serialize_item_format(f, "n-installed-jobs", "%u", m->n_installed_jobs);
        (void) serialize_item_format(f, "n-failed-jobs", "%u", m->n_failed_jobs);
        (void) serialize_bool(f, "ready-sent", m->ready_sent);
        (void) serialize_bool(f, "taint-logged", m->taint_logged);
        (void) serialize_bool(f, "service-watchdogs", m->service_watchdogs);

        if (m->show_status_overridden != _SHOW_STATUS_INVALID)
                (void) serialize_item(f, "show-status-overridden",
                                      show_status_to_string(m->show_status_overridden));

        if (m->log_level_overridden)
                (void) serialize_item_format(f, "log-level-override", "%i", log_get_max_level());
        if (m->log_target_overridden)
                (void) serialize_item(f, "log-target-override", log_target_to_string(log_get_target()));

        (void) serialize_usec(f, "runtime-watchdog-overridden", m->watchdog_overridden[WATCHDOG_RUNTIME]);
        (void) serialize_usec(f, "reboot-watchdog-overridden", m->watchdog_overridden[WATCHDOG_REBOOT]);
        (void) serialize_usec(f, "kexec-watchdog-overridden", m->watchdog_overridden[WATCHDOG_KEXEC]);
        (void) serialize_usec(f, "pretimeout-watchdog-overridden", m->watchdog_overridden[WATCHDOG_PRETIMEOUT]);
        (void) serialize_item(f, "pretimeout-watchdog-governor-overridden", m->watchdog_pretimeout_governor_overridden);

        for (ManagerTimestamp q = 0; q < _MANAGER_TIMESTAMP_MAX; q++) {
                _cleanup_free_ char *joined = NULL;

                if (!manager_timestamp_shall_serialize(q))
                        continue;

                joined = strjoin(manager_timestamp_to_string(q), "-timestamp");
                if (!joined)
                        return log_oom();

                (void) serialize_dual_timestamp(f, joined, m->timestamps + q);
        }

        if (!switching_root)
                (void) serialize_strv(f, "env", m->client_environment);

        if (m->notify_fd >= 0) {
                r = serialize_fd(f, fds, "notify-fd", m->notify_fd);
                if (r < 0)
                        return r;

                (void) serialize_item(f, "notify-socket", m->notify_socket);
        }

        if (m->cgroups_agent_fd >= 0) {
                r = serialize_fd(f, fds, "cgroups-agent-fd", m->cgroups_agent_fd);
                if (r < 0)
                        return r;
        }

        if (m->user_lookup_fds[0] >= 0) {
                int copy0, copy1;

                copy0 = fdset_put_dup(fds, m->user_lookup_fds[0]);
                if (copy0 < 0)
                        return log_error_errno(copy0, "Failed to add user lookup fd to serialization: %m");

                copy1 = fdset_put_dup(fds, m->user_lookup_fds[1]);
                if (copy1 < 0)
                        return log_error_errno(copy1, "Failed to add user lookup fd to serialization: %m");

                (void) serialize_item_format(f, "user-lookup", "%i %i", copy0, copy1);
        }

        (void) serialize_item_format(f,
                                     "dump-ratelimit",
                                     USEC_FMT " " USEC_FMT " %u %u",
                                     m->dump_ratelimit.begin,
                                     m->dump_ratelimit.interval,
                                     m->dump_ratelimit.num,
                                     m->dump_ratelimit.burst);

        bus_track_serialize(m->subscribed, f, "subscribed");

        r = dynamic_user_serialize(m, f, fds);
        if (r < 0)
                return r;

        manager_serialize_uid_refs(m, f);
        manager_serialize_gid_refs(m, f);

        r = exec_shared_runtime_serialize(m, f, fds);
        if (r < 0)
                return r;

        r = varlink_server_serialize(m->varlink_server, f, fds);
        if (r < 0)
                return r;

        (void) fputc('\n', f);

        HASHMAP_FOREACH_KEY(u, t, m->units) {
                if (u->id != t)
                        continue;

                r = unit_serialize_state(u, f, fds, switching_root);
                if (r < 0)
                        return r;
        }

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to flush serialization: %m");

        r = bus_fdset_add_all(m, fds);
        if (r < 0)
                return log_error_errno(r, "Failed to add bus sockets to serialization: %m");

        return 0;
}

static int manager_deserialize_one_unit(Manager *m, const char *name, FILE *f, FDSet *fds) {
        Unit *u;
        int r;

        r = manager_load_unit(m, name, NULL, NULL, &u);
        if (r < 0) {
                if (r == -ENOMEM)
                        return r;
                return log_notice_errno(r, "Failed to load unit \"%s\", skipping deserialization: %m", name);
        }

        r = unit_deserialize_state(u, f, fds);
        if (r < 0) {
                if (r == -ENOMEM)
                        return r;
                return log_notice_errno(r, "Failed to deserialize unit \"%s\", skipping: %m", name);
        }

        return 0;
}

static int manager_deserialize_units(Manager *m, FILE *f, FDSet *fds) {
        const char *unit_name;
        int r;

        for (;;) {
                _cleanup_free_ char *line = NULL;
                /* Start marker */
                r = read_line(f, LONG_LINE_MAX, &line);
                if (r < 0)
                        return log_error_errno(r, "Failed to read serialization line: %m");
                if (r == 0)
                        break;

                unit_name = strstrip(line);

                r = manager_deserialize_one_unit(m, unit_name, f, fds);
                if (r == -ENOMEM)
                        return r;
                if (r < 0) {
                        r = unit_deserialize_state_skip(f);
                        if (r < 0)
                                return r;
                }
        }

        return 0;
}

static void manager_deserialize_uid_refs_one_internal(
                Hashmap** uid_refs,
                const char *value) {

        uid_t uid;
        uint32_t c;
        int r;

        assert(uid_refs);
        assert(value);

        r = parse_uid(value, &uid);
        if (r < 0 || uid == 0) {
                log_debug("Unable to parse UID/GID reference serialization: %s", value);
                return;
        }

        if (hashmap_ensure_allocated(uid_refs, &trivial_hash_ops) < 0) {
                log_oom();
                return;
        }

        c = PTR_TO_UINT32(hashmap_get(*uid_refs, UID_TO_PTR(uid)));
        if (c & DESTROY_IPC_FLAG)
                return;

        c |= DESTROY_IPC_FLAG;

        r = hashmap_replace(*uid_refs, UID_TO_PTR(uid), UINT32_TO_PTR(c));
        if (r < 0) {
                log_debug_errno(r, "Failed to add UID/GID reference entry: %m");
                return;
        }
}

static void manager_deserialize_uid_refs_one(Manager *m, const char *value) {
        manager_deserialize_uid_refs_one_internal(&m->uid_refs, value);
}

static void manager_deserialize_gid_refs_one(Manager *m, const char *value) {
        manager_deserialize_uid_refs_one_internal(&m->gid_refs, value);
}

int manager_deserialize(Manager *m, FILE *f, FDSet *fds) {
        bool deserialize_varlink_sockets = false;
        int r = 0;

        assert(m);
        assert(f);

        if (DEBUG_LOGGING) {
                if (fdset_isempty(fds))
                        log_debug("No file descriptors passed");
                else {
                        int fd;

                        FDSET_FOREACH(fd, fds) {
                                _cleanup_free_ char *fn = NULL;

                                r = fd_get_path(fd, &fn);
                                if (r < 0)
                                        log_debug_errno(r, "Received serialized fd %i %s %m",
                                                        fd, special_glyph(SPECIAL_GLYPH_ARROW_RIGHT));
                                else
                                        log_debug("Received serialized fd %i %s %s",
                                                  fd, special_glyph(SPECIAL_GLYPH_ARROW_RIGHT), strna(fn));
                        }
                }
        }

        log_debug("Deserializing state...");

        /* If we are not in reload mode yet, enter it now. Not that this is recursive, a caller might already have
         * increased it to non-zero, which is why we just increase it by one here and down again at the end of this
         * call. */
        _cleanup_(manager_reloading_stopp) _unused_ Manager *reloading = manager_reloading_start(m);

        for (;;) {
                _cleanup_free_ char *l = NULL;
                const char *val;

                r = deserialize_read_line(f, &l);
                if (r < 0)
                        return r;
                if (r == 0) /* eof or end marker */
                        break;

                if ((val = startswith(l, "current-job-id="))) {
                        uint32_t id;

                        if (safe_atou32(val, &id) < 0)
                                log_notice("Failed to parse current job id value '%s', ignoring.", val);
                        else
                                m->current_job_id = MAX(m->current_job_id, id);

                } else if ((val = startswith(l, "n-installed-jobs="))) {
                        uint32_t n;

                        if (safe_atou32(val, &n) < 0)
                                log_notice("Failed to parse installed jobs counter '%s', ignoring.", val);
                        else
                                m->n_installed_jobs += n;

                } else if ((val = startswith(l, "n-failed-jobs="))) {
                        uint32_t n;

                        if (safe_atou32(val, &n) < 0)
                                log_notice("Failed to parse failed jobs counter '%s', ignoring.", val);
                        else
                                m->n_failed_jobs += n;

                } else if ((val = startswith(l, "ready-sent="))) {
                        int b;

                        b = parse_boolean(val);
                        if (b < 0)
                                log_notice("Failed to parse ready-sent flag '%s', ignoring.", val);
                        else
                                m->ready_sent = m->ready_sent || b;

                } else if ((val = startswith(l, "taint-logged="))) {
                        int b;

                        b = parse_boolean(val);
                        if (b < 0)
                                log_notice("Failed to parse taint-logged flag '%s', ignoring.", val);
                        else
                                m->taint_logged = m->taint_logged || b;

                } else if ((val = startswith(l, "service-watchdogs="))) {
                        int b;

                        b = parse_boolean(val);
                        if (b < 0)
                                log_notice("Failed to parse service-watchdogs flag '%s', ignoring.", val);
                        else
                                m->service_watchdogs = b;

                } else if ((val = startswith(l, "show-status-overridden="))) {
                        ShowStatus s;

                        s = show_status_from_string(val);
                        if (s < 0)
                                log_notice("Failed to parse show-status-overridden flag '%s', ignoring.", val);
                        else
                                manager_override_show_status(m, s, "deserialize");

                } else if ((val = startswith(l, "log-level-override="))) {
                        int level;

                        level = log_level_from_string(val);
                        if (level < 0)
                                log_notice("Failed to parse log-level-override value '%s', ignoring.", val);
                        else
                                manager_override_log_level(m, level);

                } else if ((val = startswith(l, "log-target-override="))) {
                        LogTarget target;

                        target = log_target_from_string(val);
                        if (target < 0)
                                log_notice("Failed to parse log-target-override value '%s', ignoring.", val);
                        else
                                manager_override_log_target(m, target);

                } else if ((val = startswith(l, "runtime-watchdog-overridden="))) {
                        usec_t t;

                        if (deserialize_usec(val, &t) < 0)
                                log_notice("Failed to parse runtime-watchdog-overridden value '%s', ignoring.", val);
                        else
                                manager_override_watchdog(m, WATCHDOG_RUNTIME, t);

                } else if ((val = startswith(l, "reboot-watchdog-overridden="))) {
                        usec_t t;

                        if (deserialize_usec(val, &t) < 0)
                                log_notice("Failed to parse reboot-watchdog-overridden value '%s', ignoring.", val);
                        else
                                manager_override_watchdog(m, WATCHDOG_REBOOT, t);

                } else if ((val = startswith(l, "kexec-watchdog-overridden="))) {
                        usec_t t;

                        if (deserialize_usec(val, &t) < 0)
                                log_notice("Failed to parse kexec-watchdog-overridden value '%s', ignoring.", val);
                        else
                                manager_override_watchdog(m, WATCHDOG_KEXEC, t);

                } else if ((val = startswith(l, "pretimeout-watchdog-overridden="))) {
                        usec_t t;

                        if (deserialize_usec(val, &t) < 0)
                                log_notice("Failed to parse pretimeout-watchdog-overridden value '%s', ignoring.", val);
                        else
                                manager_override_watchdog(m, WATCHDOG_PRETIMEOUT, t);

                } else if ((val = startswith(l, "pretimeout-watchdog-governor-overridden="))) {
                        r = free_and_strdup(&m->watchdog_pretimeout_governor_overridden, val);
                        if (r < 0)
                                return r;

                } else if (startswith(l, "env=")) {
                        r = deserialize_environment(l + 4, &m->client_environment);
                        if (r < 0)
                                log_notice_errno(r, "Failed to parse environment entry: \"%s\", ignoring: %m", l);

                } else if ((val = startswith(l, "notify-fd="))) {
                        int fd;

                        if ((fd = parse_fd(val)) < 0 || !fdset_contains(fds, fd))
                                log_notice("Failed to parse notify fd, ignoring: \"%s\"", val);
                        else {
                                m->notify_event_source = sd_event_source_disable_unref(m->notify_event_source);
                                safe_close(m->notify_fd);
                                m->notify_fd = fdset_remove(fds, fd);
                        }

                } else if ((val = startswith(l, "notify-socket="))) {
                        r = free_and_strdup(&m->notify_socket, val);
                        if (r < 0)
                                return r;

                } else if ((val = startswith(l, "cgroups-agent-fd="))) {
                        int fd;

                        if ((fd = parse_fd(val)) < 0 || !fdset_contains(fds, fd))
                                log_notice("Failed to parse cgroups agent fd, ignoring.: %s", val);
                        else {
                                m->cgroups_agent_event_source = sd_event_source_disable_unref(m->cgroups_agent_event_source);
                                safe_close(m->cgroups_agent_fd);
                                m->cgroups_agent_fd = fdset_remove(fds, fd);
                        }

                } else if ((val = startswith(l, "user-lookup="))) {
                        int fd0, fd1;

                        if (sscanf(val, "%i %i", &fd0, &fd1) != 2 || fd0 < 0 || fd1 < 0 || fd0 == fd1 || !fdset_contains(fds, fd0) || !fdset_contains(fds, fd1))
                                log_notice("Failed to parse user lookup fd, ignoring: %s", val);
                        else {
                                m->user_lookup_event_source = sd_event_source_disable_unref(m->user_lookup_event_source);
                                safe_close_pair(m->user_lookup_fds);
                                m->user_lookup_fds[0] = fdset_remove(fds, fd0);
                                m->user_lookup_fds[1] = fdset_remove(fds, fd1);
                        }

                } else if ((val = startswith(l, "dynamic-user=")))
                        dynamic_user_deserialize_one(m,
                                        val,
                                        fds,
                                        /* store_index= */ false,
                                        /* ret= */ NULL);
                else if ((val = startswith(l, "destroy-ipc-uid=")))
                        manager_deserialize_uid_refs_one(m, val);
                else if ((val = startswith(l, "destroy-ipc-gid=")))
                        manager_deserialize_gid_refs_one(m, val);
                else if ((val = startswith(l, "exec-runtime=")))
                        (void) exec_shared_runtime_deserialize_one(m, val, fds);
                else if ((val = startswith(l, "subscribed="))) {

                        if (strv_extend(&m->deserialized_subscribed, val) < 0)
                                return -ENOMEM;
                } else if ((val = startswith(l, "varlink-server-socket-address="))) {
                        if (!m->varlink_server && MANAGER_IS_SYSTEM(m)) {
                                _cleanup_(varlink_server_unrefp) VarlinkServer *s = NULL;

                                r = manager_setup_varlink_server(m, &s);
                                if (r < 0) {
                                        log_warning_errno(r, "Failed to setup varlink server, ignoring: %m");
                                        continue;
                                }

                                r = varlink_server_attach_event(s, m->event, SD_EVENT_PRIORITY_NORMAL);
                                if (r < 0) {
                                        log_warning_errno(r, "Failed to attach varlink connection to event loop, ignoring: %m");
                                        continue;
                                }

                                m->varlink_server = TAKE_PTR(s);
                                deserialize_varlink_sockets = true;
                        }

                        /* To void unnecessary deserialization (i.e. during reload vs. reexec) we only deserialize
                         * the FDs if we had to create a new m->varlink_server. The deserialize_varlink_sockets flag
                         * is initialized outside of the loop, is flipped after the VarlinkServer is setup, and
                         * remains set until all serialized contents are handled. */
                        if (deserialize_varlink_sockets)
                                (void) varlink_server_deserialize_one(m->varlink_server, val, fds);
                } else if ((val = startswith(l, "dump-ratelimit="))) {
                        usec_t begin, interval;
                        unsigned num, burst;

                        if (sscanf(val, USEC_FMT " " USEC_FMT " %u %u", &begin, &interval, &num, &burst) != 4)
                                log_notice("Failed to parse dump ratelimit, ignoring: %s", val);
                        else {
                                /* If we changed the values across versions, flush the counter */
                                if (interval != m->dump_ratelimit.interval || burst != m->dump_ratelimit.burst)
                                        m->dump_ratelimit.num = 0;
                                else
                                        m->dump_ratelimit.num = num;
                                m->dump_ratelimit.begin = begin;
                        }

                } else {
                        ManagerTimestamp q;

                        for (q = 0; q < _MANAGER_TIMESTAMP_MAX; q++) {
                                val = startswith(l, manager_timestamp_to_string(q));
                                if (!val)
                                        continue;

                                val = startswith(val, "-timestamp=");
                                if (val)
                                        break;
                        }

                        if (q < _MANAGER_TIMESTAMP_MAX) /* found it */
                                (void) deserialize_dual_timestamp(val, m->timestamps + q);
                        else if (!STARTSWITH_SET(l, "kdbus-fd=", "honor-device-enumeration=")) /* ignore deprecated values */
                                log_notice("Unknown serialization item '%s', ignoring.", l);
                }
        }

        return manager_deserialize_units(m, f, fds);
}
