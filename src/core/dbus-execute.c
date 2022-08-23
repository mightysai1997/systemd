/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <sys/mount.h>
#include <sys/prctl.h>

#if HAVE_SECCOMP
#include <seccomp.h>
#endif

#include "af-list.h"
#include "alloc-util.h"
#include "bus-get-properties.h"
#include "cap-list.h"
#include "capability-util.h"
#include "cpu-set-util.h"
#include "creds-util.h"
#include "dbus-execute.h"
#include "dbus-util.h"
#include "env-util.h"
#include "errno-list.h"
#include "escape.h"
#include "execute.h"
#include "fd-util.h"
#include "fileio.h"
#include "hexdecoct.h"
#include "io-util.h"
#include "ioprio-util.h"
#include "journal-file.h"
#include "missing_ioprio.h"
#include "mountpoint-util.h"
#include "namespace.h"
#include "parse-util.h"
#include "path-util.h"
#include "process-util.h"
#include "rlimit-util.h"
#if HAVE_SECCOMP
#include "seccomp-util.h"
#endif
#include "securebits-util.h"
#include "specifier.h"
#include "stat-util.h"
#include "strv.h"
#include "syslog-util.h"
#include "unit-printf.h"
#include "user-util.h"
#include "utf8.h"

BUS_DEFINE_PROPERTY_GET_ENUM(bus_property_get_exec_output, exec_output, ExecOutput);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_input, exec_input, ExecInput);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_utmp_mode, exec_utmp_mode, ExecUtmpMode);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_preserve_mode, exec_preserve_mode, ExecPreserveMode);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_exec_keyring_mode, exec_keyring_mode, ExecKeyringMode);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_protect_proc, protect_proc, ProtectProc);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_proc_subset, proc_subset, ProcSubset);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_protect_home, protect_home, ProtectHome);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_protect_system, protect_system, ProtectSystem);
static BUS_DEFINE_PROPERTY_GET_ENUM(property_get_personality, personality, unsigned long);
static BUS_DEFINE_PROPERTY_GET(property_get_ioprio, "i", ExecContext, exec_context_get_effective_ioprio);
static BUS_DEFINE_PROPERTY_GET(property_get_mount_apivfs, "b", ExecContext, exec_context_get_effective_mount_apivfs);
static BUS_DEFINE_PROPERTY_GET2(property_get_ioprio_class, "i", ExecContext, exec_context_get_effective_ioprio, ioprio_prio_class);
static BUS_DEFINE_PROPERTY_GET2(property_get_ioprio_priority, "i", ExecContext, exec_context_get_effective_ioprio, ioprio_prio_data);
static BUS_DEFINE_PROPERTY_GET_GLOBAL(property_get_empty_string, "s", NULL);
static BUS_DEFINE_PROPERTY_GET_REF(property_get_syslog_level, "i", int, LOG_PRI);
static BUS_DEFINE_PROPERTY_GET_REF(property_get_syslog_facility, "i", int, LOG_FAC);
static BUS_DEFINE_PROPERTY_GET(property_get_cpu_affinity_from_numa, "b", ExecContext, exec_context_get_cpu_affinity_from_numa);

static int property_get_environment_files(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

        r = sd_bus_message_open_container(reply, 'a', "(sb)");
        if (r < 0)
                return r;

        STRV_FOREACH(j, c->environment_files) {
                const char *fn = *j;

                r = sd_bus_message_append(reply, "(sb)", fn[0] == '-' ? fn + 1 : fn, fn[0] == '-');
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_oom_score_adjust(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int r, n;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->oom_score_adjust_set)
                n = c->oom_score_adjust;
        else {
                n = 0;
                r = get_oom_score_adjust(&n);
                if (r < 0)
                        log_debug_errno(r, "Failed to read /proc/self/oom_score_adj, ignoring: %m");
        }

        return sd_bus_message_append(reply, "i", n);
}

static int property_get_coredump_filter(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        uint64_t n;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->coredump_filter_set)
                n = c->coredump_filter;
        else {
                _cleanup_free_ char *t = NULL;

                n = COREDUMP_FILTER_MASK_DEFAULT;
                r = read_one_line_file("/proc/self/coredump_filter", &t);
                if (r < 0)
                        log_debug_errno(r, "Failed to read /proc/self/coredump_filter, ignoring: %m");
                else {
                        r = safe_atoux64(t, &n);
                        if (r < 0)
                                log_debug_errno(r, "Failed to parse \"%s\" from /proc/self/coredump_filter, ignoring: %m", t);
                }
        }

        return sd_bus_message_append(reply, "t", n);
}

static int property_get_nice(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int32_t n;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->nice_set)
                n = c->nice;
        else {
                errno = 0;
                n = getpriority(PRIO_PROCESS, 0);
                if (errno > 0)
                        n = 0;
        }

        return sd_bus_message_append(reply, "i", n);
}

static int property_get_cpu_sched_policy(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int32_t n;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->cpu_sched_set)
                n = c->cpu_sched_policy;
        else {
                n = sched_getscheduler(0);
                if (n < 0)
                        n = SCHED_OTHER;
        }

        return sd_bus_message_append(reply, "i", n);
}

static int property_get_cpu_sched_priority(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int32_t n;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->cpu_sched_set)
                n = c->cpu_sched_priority;
        else {
                struct sched_param p = {};

                if (sched_getparam(0, &p) >= 0)
                        n = p.sched_priority;
                else
                        n = 0;
        }

        return sd_bus_message_append(reply, "i", n);
}

static int property_get_cpu_affinity(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_(cpu_set_reset) CPUSet s = {};
        _cleanup_free_ uint8_t *array = NULL;
        size_t allocated;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->cpu_affinity_from_numa) {
                int r;

                r = numa_to_cpu_set(&c->numa_policy, &s);
                if (r < 0)
                        return r;
        }

        (void) cpu_set_to_dbus(c->cpu_affinity_from_numa ? &s : &c->cpu_set,  &array, &allocated);

        return sd_bus_message_append_array(reply, 'y', array, allocated);
}

static int property_get_numa_mask(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_free_ uint8_t *array = NULL;
        size_t allocated;

        assert(bus);
        assert(reply);
        assert(c);

        (void) cpu_set_to_dbus(&c->numa_policy.nodes, &array, &allocated);

        return sd_bus_message_append_array(reply, 'y', array, allocated);
}

static int property_get_numa_policy(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {
        ExecContext *c = userdata;
        int32_t policy;

        assert(bus);
        assert(reply);
        assert(c);

        policy = numa_policy_get_type(&c->numa_policy);

        return sd_bus_message_append_basic(reply, 'i', &policy);
}

static int property_get_timer_slack_nsec(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        uint64_t u;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->timer_slack_nsec != NSEC_INFINITY)
                u = (uint64_t) c->timer_slack_nsec;
        else
                u = (uint64_t) prctl(PR_GET_TIMERSLACK);

        return sd_bus_message_append(reply, "t", u);
}

static int property_get_syscall_filter(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_strv_free_ char **l = NULL;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

        r = sd_bus_message_open_container(reply, 'r', "bas");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "b", c->syscall_allow_list);
        if (r < 0)
                return r;

#if HAVE_SECCOMP
        void *id, *val;
        HASHMAP_FOREACH_KEY(val, id, c->syscall_filter) {
                _cleanup_free_ char *name = NULL;
                const char *e = NULL;
                char *s;
                int num = PTR_TO_INT(val);

                if (c->syscall_allow_list && num >= 0)
                        /* syscall with num >= 0 in allow-list is denied. */
                        continue;

                name = seccomp_syscall_resolve_num_arch(SCMP_ARCH_NATIVE, PTR_TO_INT(id) - 1);
                if (!name)
                        continue;

                if (num >= 0) {
                        e = seccomp_errno_or_action_to_string(num);
                        if (e) {
                                s = strjoin(name, ":", e);
                                if (!s)
                                        return -ENOMEM;
                        } else {
                                r = asprintf(&s, "%s:%d", name, num);
                                if (r < 0)
                                        return -ENOMEM;
                        }
                } else
                        s = TAKE_PTR(name);

                r = strv_consume(&l, s);
                if (r < 0)
                        return r;
        }
#endif

        strv_sort(l);

        r = sd_bus_message_append_strv(reply, l);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

static int property_get_syscall_log(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_strv_free_ char **l = NULL;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

        r = sd_bus_message_open_container(reply, 'r', "bas");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "b", c->syscall_log_allow_list);
        if (r < 0)
                return r;

#if HAVE_SECCOMP
        void *id, *val;
        HASHMAP_FOREACH_KEY(val, id, c->syscall_log) {
                char *name = NULL;

                name = seccomp_syscall_resolve_num_arch(SCMP_ARCH_NATIVE, PTR_TO_INT(id) - 1);
                if (!name)
                        continue;

                r = strv_consume(&l, name);
                if (r < 0)
                        return r;
        }
#endif

        strv_sort(l);

        r = sd_bus_message_append_strv(reply, l);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

static int property_get_syscall_archs(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_strv_free_ char **l = NULL;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

#if HAVE_SECCOMP
        void *id;
        SET_FOREACH(id, c->syscall_archs) {
                const char *name;

                name = seccomp_arch_to_string(PTR_TO_UINT32(id) - 1);
                if (!name)
                        continue;

                r = strv_extend(&l, name);
                if (r < 0)
                        return -ENOMEM;
        }
#endif

        strv_sort(l);

        r = sd_bus_message_append_strv(reply, l);
        if (r < 0)
                return r;

        return 0;
}

static int property_get_selinux_context(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "(bs)", c->selinux_context_ignore, c->selinux_context);
}

static int property_get_apparmor_profile(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "(bs)", c->apparmor_profile_ignore, c->apparmor_profile);
}

static int property_get_smack_process_label(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(reply);
        assert(c);

        return sd_bus_message_append(reply, "(bs)", c->smack_process_label_ignore, c->smack_process_label);
}

static int property_get_address_families(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_strv_free_ char **l = NULL;
        void *af;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

        r = sd_bus_message_open_container(reply, 'r', "bas");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "b", c->address_families_allow_list);
        if (r < 0)
                return r;

        SET_FOREACH(af, c->address_families) {
                const char *name;

                name = af_to_name(PTR_TO_INT(af));
                if (!name)
                        continue;

                r = strv_extend(&l, name);
                if (r < 0)
                        return -ENOMEM;
        }

        strv_sort(l);

        r = sd_bus_message_append_strv(reply, l);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

static int property_get_working_directory(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        const char *wd;

        assert(bus);
        assert(reply);
        assert(c);

        if (c->working_directory_home)
                wd = "~";
        else
                wd = c->working_directory;

        if (c->working_directory_missing_ok)
                wd = strjoina("!", wd);

        return sd_bus_message_append(reply, "s", wd);
}

static int property_get_stdio_fdname(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int fileno;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        if (streq(property, "StandardInputFileDescriptorName"))
                fileno = STDIN_FILENO;
        else if (streq(property, "StandardOutputFileDescriptorName"))
                fileno = STDOUT_FILENO;
        else {
                assert(streq(property, "StandardErrorFileDescriptorName"));
                fileno = STDERR_FILENO;
        }

        return sd_bus_message_append(reply, "s", exec_context_fdname(c, fileno));
}

static int property_get_input_data(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        return sd_bus_message_append_array(reply, 'y', c->stdin_data, c->stdin_data_size);
}

static int property_get_restrict_filesystems(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        _cleanup_free_ char **l = NULL;
        int r;

        assert(bus);
        assert(reply);
        assert(c);

        r = sd_bus_message_open_container(reply, 'r', "bas");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "b", c->restrict_filesystems_allow_list);
        if (r < 0)
                return r;

#if HAVE_LIBBPF
        l = set_get_strv(c->restrict_filesystems);
        if (!l)
                return -ENOMEM;
#endif

        strv_sort(l);

        r = sd_bus_message_append_strv(reply, l);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

static int property_get_bind_paths(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        bool ro;
        int r;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        ro = strstr(property, "ReadOnly");

        r = sd_bus_message_open_container(reply, 'a', "(ssbt)");
        if (r < 0)
                return r;

        for (size_t i = 0; i < c->n_bind_mounts; i++) {

                if (ro != c->bind_mounts[i].read_only)
                        continue;

                r = sd_bus_message_append(
                                reply, "(ssbt)",
                                c->bind_mounts[i].source,
                                c->bind_mounts[i].destination,
                                c->bind_mounts[i].ignore_enoent,
                                c->bind_mounts[i].recursive ? (uint64_t) MS_REC : (uint64_t) 0);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_temporary_filesystems(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int r;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(ss)");
        if (r < 0)
                return r;

        for (unsigned i = 0; i < c->n_temporary_filesystems; i++) {
                TemporaryFileSystem *t = c->temporary_filesystems + i;

                r = sd_bus_message_append(
                                reply, "(ss)",
                                t->path,
                                t->options);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_log_extra_fields(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int r;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "ay");
        if (r < 0)
                return r;

        for (size_t i = 0; i < c->n_log_extra_fields; i++) {
                r = sd_bus_message_append_array(reply, 'y', c->log_extra_fields[i].iov_base, c->log_extra_fields[i].iov_len);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_set_credential(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        ExecSetCredential *sc;
        int r;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(say)");
        if (r < 0)
                return r;

        HASHMAP_FOREACH(sc, c->set_credentials) {

                if (sc->encrypted != streq(property, "SetCredentialEncrypted"))
                        continue;

                r = sd_bus_message_open_container(reply, 'r', "say");
                if (r < 0)
                        return r;

                r = sd_bus_message_append(reply, "s", sc->id);
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', sc->data, sc->size);
                if (r < 0)
                        return r;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_load_credential(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        ExecLoadCredential *lc;
        int r;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(ss)");
        if (r < 0)
                return r;

        HASHMAP_FOREACH(lc, c->load_credentials) {

                if (lc->encrypted != streq(property, "LoadCredentialEncrypted"))
                        continue;

                r = sd_bus_message_append(reply, "(ss)", lc->id, lc->path);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_root_hash(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        return sd_bus_message_append_array(reply, 'y', c->root_hash, c->root_hash_size);
}

static int property_get_root_hash_sig(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        return sd_bus_message_append_array(reply, 'y', c->root_hash_sig, c->root_hash_sig_size);
}

static int property_get_root_image_options(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int r;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(ss)");
        if (r < 0)
                return r;

        LIST_FOREACH(mount_options, m, c->root_image_options) {
                r = sd_bus_message_append(reply, "(ss)",
                                          partition_designator_to_string(m->partition_designator),
                                          m->options);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_mount_images(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int r;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(ssba(ss))");
        if (r < 0)
                return r;

        for (size_t i = 0; i < c->n_mount_images; i++) {
                r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "ssba(ss)");
                if (r < 0)
                        return r;
                r = sd_bus_message_append(
                                reply, "ssb",
                                c->mount_images[i].source,
                                c->mount_images[i].destination,
                                c->mount_images[i].ignore_enoent);
                if (r < 0)
                        return r;
                r = sd_bus_message_open_container(reply, 'a', "(ss)");
                if (r < 0)
                        return r;
                LIST_FOREACH(mount_options, m, c->mount_images[i].mount_options) {
                        r = sd_bus_message_append(reply, "(ss)",
                                                  partition_designator_to_string(m->partition_designator),
                                                  m->options);
                        if (r < 0)
                                return r;
                }
                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int property_get_extension_images(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecContext *c = userdata;
        int r;

        assert(bus);
        assert(c);
        assert(property);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(sba(ss))");
        if (r < 0)
                return r;

        for (size_t i = 0; i < c->n_extension_images; i++) {
                r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "sba(ss)");
                if (r < 0)
                        return r;
                r = sd_bus_message_append(
                                reply, "sb",
                                c->extension_images[i].source,
                                c->extension_images[i].ignore_enoent);
                if (r < 0)
                        return r;
                r = sd_bus_message_open_container(reply, 'a', "(ss)");
                if (r < 0)
                        return r;
                LIST_FOREACH(mount_options, m, c->extension_images[i].mount_options) {
                        r = sd_bus_message_append(reply, "(ss)",
                                                  partition_designator_to_string(m->partition_designator),
                                                  m->options);
                        if (r < 0)
                                return r;
                }
                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int bus_property_get_exec_dir(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecDirectory *d = userdata;
        int r;

        assert(bus);
        assert(d);
        assert(property);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "s");
        if (r < 0)
                return r;

        for (size_t i = 0; i < d->n_items; i++) {
                r = sd_bus_message_append_basic(reply, 's', d->items[i].path);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int bus_property_get_exec_dir_symlink(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        ExecDirectory *d = userdata;
        int r;

        assert(bus);
        assert(d);
        assert(property);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(sst)");
        if (r < 0)
                return r;

        for (size_t i = 0; i < d->n_items; i++)
                STRV_FOREACH(dst, d->items[i].symlinks) {
                        r = sd_bus_message_append(reply, "(sst)", d->items[i].path, *dst, 0 /* flags, unused for now */);
                        if (r < 0)
                                return r;
                }

        return sd_bus_message_close_container(reply);
}

const sd_bus_vtable bus_exec_vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_PROPERTY("Environment", "as", NULL, offsetof(ExecContext, environment), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("EnvironmentFiles", "a(sb)", property_get_environment_files, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PassEnvironment", "as", NULL, offsetof(ExecContext, pass_environment), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UnsetEnvironment", "as", NULL, offsetof(ExecContext, unset_environment), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UMask", "u", bus_property_get_mode, offsetof(ExecContext, umask), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitCPU", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_CPU]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitCPUSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_CPU]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitFSIZE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_FSIZE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitFSIZESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_FSIZE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitDATA", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_DATA]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitDATASoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_DATA]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitSTACK", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_STACK]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitSTACKSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_STACK]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitCORE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_CORE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitCORESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_CORE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRSS", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RSS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRSSSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RSS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNOFILE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NOFILE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNOFILESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NOFILE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitAS", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_AS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitASSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_AS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNPROC", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NPROC]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNPROCSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NPROC]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitMEMLOCK", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_MEMLOCK]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitMEMLOCKSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_MEMLOCK]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitLOCKS", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_LOCKS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitLOCKSSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_LOCKS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitSIGPENDING", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_SIGPENDING]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitSIGPENDINGSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_SIGPENDING]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitMSGQUEUE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_MSGQUEUE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitMSGQUEUESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_MSGQUEUE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNICE", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NICE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitNICESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_NICE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRTPRIO", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RTPRIO]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRTPRIOSoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RTPRIO]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRTTIME", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RTTIME]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LimitRTTIMESoft", "t", bus_property_get_rlimit, offsetof(ExecContext, rlimit[RLIMIT_RTTIME]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("WorkingDirectory", "s", property_get_working_directory, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootDirectory", "s", NULL, offsetof(ExecContext, root_directory), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootImage", "s", NULL, offsetof(ExecContext, root_image), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootImageOptions", "a(ss)", property_get_root_image_options, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootHash", "ay", property_get_root_hash, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootHashPath", "s", NULL, offsetof(ExecContext, root_hash_path), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootHashSignature", "ay", property_get_root_hash_sig, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootHashSignaturePath", "s", NULL, offsetof(ExecContext, root_hash_sig_path), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RootVerity", "s", NULL, offsetof(ExecContext, root_verity), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ExtensionDirectories", "as", NULL, offsetof(ExecContext, extension_directories), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ExtensionImages", "a(sba(ss))", property_get_extension_images, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("MountImages", "a(ssba(ss))", property_get_mount_images, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("OOMScoreAdjust", "i", property_get_oom_score_adjust, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CoredumpFilter", "t", property_get_coredump_filter, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Nice", "i", property_get_nice, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("IOSchedulingClass", "i", property_get_ioprio_class, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("IOSchedulingPriority", "i", property_get_ioprio_priority, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CPUSchedulingPolicy", "i", property_get_cpu_sched_policy, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CPUSchedulingPriority", "i", property_get_cpu_sched_priority, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CPUAffinity", "ay", property_get_cpu_affinity, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CPUAffinityFromNUMA", "b", property_get_cpu_affinity_from_numa, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NUMAPolicy", "i", property_get_numa_policy, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NUMAMask", "ay", property_get_numa_mask, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TimerSlackNSec", "t", property_get_timer_slack_nsec, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CPUSchedulingResetOnFork", "b", bus_property_get_bool, offsetof(ExecContext, cpu_sched_reset_on_fork), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NonBlocking", "b", bus_property_get_bool, offsetof(ExecContext, non_blocking), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardInput", "s", property_get_exec_input, offsetof(ExecContext, std_input), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardInputFileDescriptorName", "s", property_get_stdio_fdname, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardInputData", "ay", property_get_input_data, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardOutput", "s", bus_property_get_exec_output, offsetof(ExecContext, std_output), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardOutputFileDescriptorName", "s", property_get_stdio_fdname, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardError", "s", bus_property_get_exec_output, offsetof(ExecContext, std_error), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StandardErrorFileDescriptorName", "s", property_get_stdio_fdname, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYPath", "s", NULL, offsetof(ExecContext, tty_path), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYReset", "b", bus_property_get_bool, offsetof(ExecContext, tty_reset), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYVHangup", "b", bus_property_get_bool, offsetof(ExecContext, tty_vhangup), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYVTDisallocate", "b", bus_property_get_bool, offsetof(ExecContext, tty_vt_disallocate), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYRows", "q", bus_property_get_unsigned, offsetof(ExecContext, tty_rows), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TTYColumns", "q", bus_property_get_unsigned, offsetof(ExecContext, tty_cols), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogPriority", "i", bus_property_get_int, offsetof(ExecContext, syslog_priority), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogIdentifier", "s", NULL, offsetof(ExecContext, syslog_identifier), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogLevelPrefix", "b", bus_property_get_bool, offsetof(ExecContext, syslog_level_prefix), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogLevel", "i", property_get_syslog_level, offsetof(ExecContext, syslog_priority), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SyslogFacility", "i", property_get_syslog_facility, offsetof(ExecContext, syslog_priority), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogLevelMax", "i", bus_property_get_int, offsetof(ExecContext, log_level_max), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogRateLimitIntervalUSec", "t", bus_property_get_usec, offsetof(ExecContext, log_ratelimit_interval_usec), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogRateLimitBurst", "u", bus_property_get_unsigned, offsetof(ExecContext, log_ratelimit_burst), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogExtraFields", "aay", property_get_log_extra_fields, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogNamespace", "s", NULL, offsetof(ExecContext, log_namespace), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SecureBits", "i", bus_property_get_int, offsetof(ExecContext, secure_bits), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CapabilityBoundingSet", "t", NULL, offsetof(ExecContext, capability_bounding_set), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("AmbientCapabilities", "t", NULL, offsetof(ExecContext, capability_ambient_set), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("User", "s", NULL, offsetof(ExecContext, user), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Group", "s", NULL, offsetof(ExecContext, group), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("DynamicUser", "b", bus_property_get_bool, offsetof(ExecContext, dynamic_user), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RemoveIPC", "b", bus_property_get_bool, offsetof(ExecContext, remove_ipc), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SetCredential", "a(say)", property_get_set_credential, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SetCredentialEncrypted", "a(say)", property_get_set_credential, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LoadCredential", "a(ss)", property_get_load_credential, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LoadCredentialEncrypted", "a(ss)", property_get_load_credential, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SupplementaryGroups", "as", NULL, offsetof(ExecContext, supplementary_groups), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PAMName", "s", NULL, offsetof(ExecContext, pam_name), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ReadWritePaths", "as", NULL, offsetof(ExecContext, read_write_paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ReadOnlyPaths", "as", NULL, offsetof(ExecContext, read_only_paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("InaccessiblePaths", "as", NULL, offsetof(ExecContext, inaccessible_paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ExecPaths", "as", NULL, offsetof(ExecContext, exec_paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NoExecPaths", "as", NULL, offsetof(ExecContext, no_exec_paths), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ExecSearchPath", "as", NULL, offsetof(ExecContext, exec_search_path), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("MountFlags", "t", bus_property_get_ulong, offsetof(ExecContext, mount_flags), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateTmp", "b", bus_property_get_bool, offsetof(ExecContext, private_tmp), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateDevices", "b", bus_property_get_bool, offsetof(ExecContext, private_devices), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectClock", "b", bus_property_get_bool, offsetof(ExecContext, protect_clock), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectKernelTunables", "b", bus_property_get_bool, offsetof(ExecContext, protect_kernel_tunables), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectKernelModules", "b", bus_property_get_bool, offsetof(ExecContext, protect_kernel_modules), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectKernelLogs", "b", bus_property_get_bool, offsetof(ExecContext, protect_kernel_logs), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectControlGroups", "b", bus_property_get_bool, offsetof(ExecContext, protect_control_groups), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateNetwork", "b", bus_property_get_bool, offsetof(ExecContext, private_network), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateUsers", "b", bus_property_get_bool, offsetof(ExecContext, private_users), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateMounts", "b", bus_property_get_bool, offsetof(ExecContext, private_mounts), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("PrivateIPC", "b", bus_property_get_bool, offsetof(ExecContext, private_ipc), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectHome", "s", property_get_protect_home, offsetof(ExecContext, protect_home), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectSystem", "s", property_get_protect_system, offsetof(ExecContext, protect_system), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SameProcessGroup", "b", bus_property_get_bool, offsetof(ExecContext, same_pgrp), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UtmpIdentifier", "s", NULL, offsetof(ExecContext, utmp_id), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("UtmpMode", "s", property_get_exec_utmp_mode, offsetof(ExecContext, utmp_mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SELinuxContext", "(bs)", property_get_selinux_context, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("AppArmorProfile", "(bs)", property_get_apparmor_profile, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SmackProcessLabel", "(bs)", property_get_smack_process_label, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("IgnoreSIGPIPE", "b", bus_property_get_bool, offsetof(ExecContext, ignore_sigpipe), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NoNewPrivileges", "b", bus_property_get_bool, offsetof(ExecContext, no_new_privileges), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SystemCallFilter", "(bas)", property_get_syscall_filter, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SystemCallArchitectures", "as", property_get_syscall_archs, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SystemCallErrorNumber", "i", bus_property_get_int, offsetof(ExecContext, syscall_errno), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("SystemCallLog", "(bas)", property_get_syscall_log, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("Personality", "s", property_get_personality, offsetof(ExecContext, personality), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LockPersonality", "b", bus_property_get_bool, offsetof(ExecContext, lock_personality), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictAddressFamilies", "(bas)", property_get_address_families, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RuntimeDirectorySymlink", "a(sst)", bus_property_get_exec_dir_symlink, offsetof(ExecContext, directories[EXEC_DIRECTORY_RUNTIME]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RuntimeDirectoryPreserve", "s", property_get_exec_preserve_mode, offsetof(ExecContext, runtime_directory_preserve_mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RuntimeDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_RUNTIME].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RuntimeDirectory", "as", bus_property_get_exec_dir, offsetof(ExecContext, directories[EXEC_DIRECTORY_RUNTIME]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StateDirectorySymlink", "a(sst)", bus_property_get_exec_dir_symlink, offsetof(ExecContext, directories[EXEC_DIRECTORY_STATE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StateDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_STATE].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("StateDirectory", "as", bus_property_get_exec_dir, offsetof(ExecContext, directories[EXEC_DIRECTORY_STATE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CacheDirectorySymlink", "a(sst)", bus_property_get_exec_dir_symlink, offsetof(ExecContext, directories[EXEC_DIRECTORY_CACHE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CacheDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_CACHE].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("CacheDirectory", "as", bus_property_get_exec_dir, offsetof(ExecContext, directories[EXEC_DIRECTORY_CACHE]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogsDirectorySymlink", "a(sst)", bus_property_get_exec_dir_symlink, offsetof(ExecContext, directories[EXEC_DIRECTORY_LOGS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogsDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_LOGS].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("LogsDirectory", "as", bus_property_get_exec_dir, offsetof(ExecContext, directories[EXEC_DIRECTORY_LOGS]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ConfigurationDirectoryMode", "u", bus_property_get_mode, offsetof(ExecContext, directories[EXEC_DIRECTORY_CONFIGURATION].mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ConfigurationDirectory", "as", bus_property_get_exec_dir, offsetof(ExecContext, directories[EXEC_DIRECTORY_CONFIGURATION]), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TimeoutCleanUSec", "t", bus_property_get_usec, offsetof(ExecContext, timeout_clean_usec), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("MemoryDenyWriteExecute", "b", bus_property_get_bool, offsetof(ExecContext, memory_deny_write_execute), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictRealtime", "b", bus_property_get_bool, offsetof(ExecContext, restrict_realtime), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictSUIDSGID", "b", bus_property_get_bool, offsetof(ExecContext, restrict_suid_sgid), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictNamespaces", "t", bus_property_get_ulong, offsetof(ExecContext, restrict_namespaces), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("RestrictFileSystems", "(bas)", property_get_restrict_filesystems, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("BindPaths", "a(ssbt)", property_get_bind_paths, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("BindReadOnlyPaths", "a(ssbt)", property_get_bind_paths, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("TemporaryFileSystem", "a(ss)", property_get_temporary_filesystems, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("MountAPIVFS", "b", property_get_mount_apivfs, 0, SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("KeyringMode", "s", property_get_exec_keyring_mode, offsetof(ExecContext, keyring_mode), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectProc", "s", property_get_protect_proc, offsetof(ExecContext, protect_proc), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProcSubset", "s", property_get_proc_subset, offsetof(ExecContext, proc_subset), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("ProtectHostname", "b", bus_property_get_bool, offsetof(ExecContext, protect_hostname), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("NetworkNamespacePath", "s", NULL, offsetof(ExecContext, network_namespace_path), SD_BUS_VTABLE_PROPERTY_CONST),
        SD_BUS_PROPERTY("IPCNamespacePath", "s", NULL, offsetof(ExecContext, ipc_namespace_path), SD_BUS_VTABLE_PROPERTY_CONST),

        /* Obsolete/redundant properties: */
        SD_BUS_PROPERTY("Capabilities", "s", property_get_empty_string, 0, SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("ReadWriteDirectories", "as", NULL, offsetof(ExecContext, read_write_paths), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("ReadOnlyDirectories", "as", NULL, offsetof(ExecContext, read_only_paths), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("InaccessibleDirectories", "as", NULL, offsetof(ExecContext, inaccessible_paths), SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),
        SD_BUS_PROPERTY("IOScheduling", "i", property_get_ioprio, 0, SD_BUS_VTABLE_PROPERTY_CONST|SD_BUS_VTABLE_HIDDEN),

        SD_BUS_VTABLE_END
};

static int append_exec_command(sd_bus_message *reply, ExecCommand *c) {
        int r;

        assert(reply);
        assert(c);

        if (!c->path)
                return 0;

        r = sd_bus_message_open_container(reply, 'r', "sasbttttuii");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "s", c->path);
        if (r < 0)
                return r;

        r = sd_bus_message_append_strv(reply, c->argv);
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "bttttuii",
                                  !!(c->flags & EXEC_COMMAND_IGNORE_FAILURE),
                                  c->exec_status.start_timestamp.realtime,
                                  c->exec_status.start_timestamp.monotonic,
                                  c->exec_status.exit_timestamp.realtime,
                                  c->exec_status.exit_timestamp.monotonic,
                                  (uint32_t) c->exec_status.pid,
                                  (int32_t) c->exec_status.code,
                                  (int32_t) c->exec_status.status);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

static int append_exec_ex_command(sd_bus_message *reply, ExecCommand *c) {
        _cleanup_strv_free_ char **ex_opts = NULL;
        int r;

        assert(reply);
        assert(c);

        if (!c->path)
                return 0;

        r = sd_bus_message_open_container(reply, 'r', "sasasttttuii");
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "s", c->path);
        if (r < 0)
                return r;

        r = sd_bus_message_append_strv(reply, c->argv);
        if (r < 0)
                return r;

        r = exec_command_flags_to_strv(c->flags, &ex_opts);
        if (r < 0)
                return r;

        r = sd_bus_message_append_strv(reply, ex_opts);
        if (r < 0)
                return r;

        r = sd_bus_message_append(reply, "ttttuii",
                                  c->exec_status.start_timestamp.realtime,
                                  c->exec_status.start_timestamp.monotonic,
                                  c->exec_status.exit_timestamp.realtime,
                                  c->exec_status.exit_timestamp.monotonic,
                                  (uint32_t) c->exec_status.pid,
                                  (int32_t) c->exec_status.code,
                                  (int32_t) c->exec_status.status);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

int bus_property_get_exec_command(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        ExecCommand *c = (ExecCommand*) userdata;
        int r;

        assert(bus);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(sasbttttuii)");
        if (r < 0)
                return r;

        r = append_exec_command(reply, c);
        if (r < 0)
                return r;

        return sd_bus_message_close_container(reply);
}

int bus_property_get_exec_command_list(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        ExecCommand *exec_command = *(ExecCommand**) userdata;
        int r;

        assert(bus);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(sasbttttuii)");
        if (r < 0)
                return r;

        LIST_FOREACH(command, c, exec_command) {
                r = append_exec_command(reply, c);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

int bus_property_get_exec_ex_command_list(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *ret_error) {

        ExecCommand *exec_command = *(ExecCommand**) userdata;
        int r;

        assert(bus);
        assert(reply);

        r = sd_bus_message_open_container(reply, 'a', "(sasasttttuii)");
        if (r < 0)
                return r;

        LIST_FOREACH(command, c, exec_command) {
                r = append_exec_ex_command(reply, c);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static char *exec_command_flags_to_exec_chars(ExecCommandFlags flags) {
        return strjoin(FLAGS_SET(flags, EXEC_COMMAND_IGNORE_FAILURE)   ? "-" : "",
                       FLAGS_SET(flags, EXEC_COMMAND_NO_ENV_EXPAND)    ? ":" : "",
                       FLAGS_SET(flags, EXEC_COMMAND_FULLY_PRIVILEGED) ? "+" : "",
                       FLAGS_SET(flags, EXEC_COMMAND_NO_SETUID)        ? "!" : "",
                       FLAGS_SET(flags, EXEC_COMMAND_AMBIENT_MAGIC)    ? "!!" : "");
}

int bus_set_transient_exec_command(
                Unit *u,
                const char *name,
                ExecCommand **exec_command,
                sd_bus_message *message,
                UnitWriteFlags flags,
                sd_bus_error *error) {
        bool is_ex_prop = endswith(name, "Ex");
        unsigned n = 0;
        int r;

        r = sd_bus_message_enter_container(message, 'a', is_ex_prop ? "(sasas)" : "(sasb)");
        if (r < 0)
                return r;

        while ((r = sd_bus_message_enter_container(message, 'r', is_ex_prop ? "sasas" : "sasb")) > 0) {
                _cleanup_strv_free_ char **argv = NULL, **ex_opts = NULL;
                const char *path;
                int b;

                r = sd_bus_message_read(message, "s", &path);
                if (r < 0)
                        return r;

                if (!path_is_absolute(path) && !filename_is_valid(path))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                                                 "\"%s\" is neither a valid executable name nor an absolute path",
                                                 path);

                r = sd_bus_message_read_strv(message, &argv);
                if (r < 0)
                        return r;

                if (strv_isempty(argv))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                                                 "\"%s\" argv cannot be empty", name);

                r = is_ex_prop ? sd_bus_message_read_strv(message, &ex_opts) : sd_bus_message_read(message, "b", &b);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        ExecCommand *c;

                        c = new0(ExecCommand, 1);
                        if (!c)
                                return -ENOMEM;

                        c->path = strdup(path);
                        if (!c->path) {
                                free(c);
                                return -ENOMEM;
                        }

                        c->argv = TAKE_PTR(argv);

                        if (is_ex_prop) {
                                r = exec_command_flags_from_strv(ex_opts, &c->flags);
                                if (r < 0)
                                        return r;
                        } else
                                c->flags = b ? EXEC_COMMAND_IGNORE_FAILURE : 0;

                        path_simplify(c->path);
                        exec_command_append_list(exec_command, c);
                }

                n++;
        }
        if (r < 0)
                return r;

        r = sd_bus_message_exit_container(message);
        if (r < 0)
                return r;

        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                _cleanup_free_ char *buf = NULL;
                _cleanup_fclose_ FILE *f = NULL;
                size_t size = 0;

                if (n == 0)
                        *exec_command = exec_command_free_list(*exec_command);

                f = open_memstream_unlocked(&buf, &size);
                if (!f)
                        return -ENOMEM;

                fprintf(f, "%s=\n", name);

                LIST_FOREACH(command, c, *exec_command) {
                        _cleanup_free_ char *a = NULL, *exec_chars = NULL;

                        exec_chars = exec_command_flags_to_exec_chars(c->flags);
                        if (!exec_chars)
                                return -ENOMEM;

                        a = unit_concat_strv(c->argv, UNIT_ESCAPE_SPECIFIERS|UNIT_ESCAPE_EXEC_SYNTAX);
                        if (!a)
                                return -ENOMEM;

                        if (streq_ptr(c->path, c->argv ? c->argv[0] : NULL))
                                fprintf(f, "%s=%s%s\n", name, exec_chars, a);
                        else {
                                _cleanup_free_ char *t = NULL;
                                const char *p;

                                p = unit_escape_setting(c->path,
                                                        UNIT_ESCAPE_SPECIFIERS|UNIT_ESCAPE_EXEC_SYNTAX, &t);
                                if (!p)
                                        return -ENOMEM;

                                fprintf(f, "%s=%s@%s %s\n", name, exec_chars, p, a);
                        }
                }

                r = fflush_and_check(f);
                if (r < 0)
                        return r;

                unit_write_setting(u, flags, name, buf);
        }

        return 1;
}

static int parse_personality(const char *s, unsigned long *p) {
        unsigned long v;

        assert(p);

        v = personality_from_string(s);
        if (v == PERSONALITY_INVALID)
                return -EINVAL;

        *p = v;
        return 0;
}

static const char* mount_propagation_flags_to_string_with_check(unsigned long n) {
        if (!IN_SET(n, 0, MS_SHARED, MS_PRIVATE, MS_SLAVE))
                return NULL;

        return mount_propagation_flags_to_string(n);
}

static BUS_DEFINE_SET_TRANSIENT(nsec, "t", uint64_t, nsec_t, NSEC_FMT);
static BUS_DEFINE_SET_TRANSIENT_IS_VALID(log_level, "i", int32_t, int, "%" PRIi32, log_level_is_valid);
#if HAVE_SECCOMP
static BUS_DEFINE_SET_TRANSIENT_IS_VALID(errno, "i", int32_t, int, "%" PRIi32, seccomp_errno_or_action_is_valid);
#endif
static BUS_DEFINE_SET_TRANSIENT_PARSE(std_input, ExecInput, exec_input_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(std_output, ExecOutput, exec_output_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(utmp_mode, ExecUtmpMode, exec_utmp_mode_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(protect_system, ProtectSystem, protect_system_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(protect_home, ProtectHome, protect_home_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(keyring_mode, ExecKeyringMode, exec_keyring_mode_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(protect_proc, ProtectProc, protect_proc_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(proc_subset, ProcSubset, proc_subset_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE(preserve_mode, ExecPreserveMode, exec_preserve_mode_from_string);
static BUS_DEFINE_SET_TRANSIENT_PARSE_PTR(personality, unsigned long, parse_personality);
static BUS_DEFINE_SET_TRANSIENT_TO_STRING_ALLOC(secure_bits, "i", int32_t, int, "%" PRIi32, secure_bits_to_string_alloc_with_check);
static BUS_DEFINE_SET_TRANSIENT_TO_STRING_ALLOC(capability, "t", uint64_t, uint64_t, "%" PRIu64, capability_set_to_string_alloc);
static BUS_DEFINE_SET_TRANSIENT_TO_STRING_ALLOC(namespace_flag, "t", uint64_t, unsigned long, "%" PRIu64, namespace_flags_to_string);
static BUS_DEFINE_SET_TRANSIENT_TO_STRING(mount_flags, "t", uint64_t, unsigned long, "%" PRIu64, mount_propagation_flags_to_string_with_check);

int bus_exec_context_set_transient_property(
                Unit *u,
                ExecContext *c,
                const char *name,
                sd_bus_message *message,
                UnitWriteFlags flags,
                sd_bus_error *error) {

        const char *suffix;
        int r;

        assert(u);
        assert(c);
        assert(name);
        assert(message);

        flags |= UNIT_PRIVATE;

        if (streq(name, "User"))
                return bus_set_transient_user_relaxed(u, name, &c->user, message, flags, error);

        if (streq(name, "Group"))
                return bus_set_transient_user_relaxed(u, name, &c->group, message, flags, error);

        if (streq(name, "TTYPath"))
                return bus_set_transient_path(u, name, &c->tty_path, message, flags, error);

        if (streq(name, "RootImage"))
                return bus_set_transient_path(u, name, &c->root_image, message, flags, error);

        if (streq(name, "RootImageOptions")) {
                _cleanup_(mount_options_free_allp) MountOptions *options = NULL;
                _cleanup_free_ char *format_str = NULL;

                r = bus_read_mount_options(message, error, &options, &format_str, " ");
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (options) {
                                LIST_JOIN(mount_options, c->root_image_options, options);
                                unit_write_settingf(
                                                u, flags|UNIT_ESCAPE_SPECIFIERS, name,
                                                "%s=%s",
                                                name,
                                                format_str);
                        } else {
                                c->root_image_options = mount_options_free_all(c->root_image_options);
                                unit_write_settingf(u, flags, name, "%s=", name);
                        }
                }

                return 1;
        }

        if (streq(name, "RootHash")) {
                const void *roothash_decoded;
                size_t roothash_decoded_size;

                r = sd_bus_message_read_array(message, 'y', &roothash_decoded, &roothash_decoded_size);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *encoded = NULL;

                        if (roothash_decoded_size == 0) {
                                c->root_hash_path = mfree(c->root_hash_path);
                                c->root_hash = mfree(c->root_hash);
                                c->root_hash_size = 0;

                                unit_write_settingf(u, flags, name, "RootHash=");
                        } else {
                                _cleanup_free_ void *p = NULL;

                                encoded = hexmem(roothash_decoded, roothash_decoded_size);
                                if (!encoded)
                                        return -ENOMEM;

                                p = memdup(roothash_decoded, roothash_decoded_size);
                                if (!p)
                                        return -ENOMEM;

                                free_and_replace(c->root_hash, p);
                                c->root_hash_size = roothash_decoded_size;
                                c->root_hash_path = mfree(c->root_hash_path);

                                unit_write_settingf(u, flags, name, "RootHash=%s", encoded);
                        }
                }

                return 1;
        }

        if (streq(name, "RootHashPath")) {
                c->root_hash_size = 0;
                c->root_hash = mfree(c->root_hash);

                return bus_set_transient_path(u, "RootHash", &c->root_hash_path, message, flags, error);
        }

        if (streq(name, "RootHashSignature")) {
                const void *roothash_sig_decoded;
                size_t roothash_sig_decoded_size;

                r = sd_bus_message_read_array(message, 'y', &roothash_sig_decoded, &roothash_sig_decoded_size);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *encoded = NULL;

                        if (roothash_sig_decoded_size == 0) {
                                c->root_hash_sig_path = mfree(c->root_hash_sig_path);
                                c->root_hash_sig = mfree(c->root_hash_sig);
                                c->root_hash_sig_size = 0;

                                unit_write_settingf(u, flags, name, "RootHashSignature=");
                        } else {
                                _cleanup_free_ void *p = NULL;
                                ssize_t len;

                                len = base64mem(roothash_sig_decoded, roothash_sig_decoded_size, &encoded);
                                if (len < 0)
                                        return -ENOMEM;

                                p = memdup(roothash_sig_decoded, roothash_sig_decoded_size);
                                if (!p)
                                        return -ENOMEM;

                                free_and_replace(c->root_hash_sig, p);
                                c->root_hash_sig_size = roothash_sig_decoded_size;
                                c->root_hash_sig_path = mfree(c->root_hash_sig_path);

                                unit_write_settingf(u, flags, name, "RootHashSignature=base64:%s", encoded);
                        }
                }

                return 1;
        }

        if (streq(name, "RootHashSignaturePath")) {
                c->root_hash_sig_size = 0;
                c->root_hash_sig = mfree(c->root_hash_sig);

                return bus_set_transient_path(u, "RootHashSignature", &c->root_hash_sig_path, message, flags, error);
        }

        if (streq(name, "RootVerity"))
                return bus_set_transient_path(u, name, &c->root_verity, message, flags, error);

        if (streq(name, "RootDirectory"))
                return bus_set_transient_path(u, name, &c->root_directory, message, flags, error);

        if (streq(name, "SyslogIdentifier"))
                return bus_set_transient_string(u, name, &c->syslog_identifier, message, flags, error);

        if (streq(name, "LogLevelMax"))
                return bus_set_transient_log_level(u, name, &c->log_level_max, message, flags, error);

        if (streq(name, "LogRateLimitIntervalUSec"))
                return bus_set_transient_usec(u, name, &c->log_ratelimit_interval_usec, message, flags, error);

        if (streq(name, "LogRateLimitBurst"))
                return bus_set_transient_unsigned(u, name, &c->log_ratelimit_burst, message, flags, error);

        if (streq(name, "Personality"))
                return bus_set_transient_personality(u, name, &c->personality, message, flags, error);

        if (streq(name, "StandardInput"))
                return bus_set_transient_std_input(u, name, &c->std_input, message, flags, error);

        if (streq(name, "StandardOutput"))
                return bus_set_transient_std_output(u, name, &c->std_output, message, flags, error);

        if (streq(name, "StandardError"))
                return bus_set_transient_std_output(u, name, &c->std_error, message, flags, error);

        if (streq(name, "IgnoreSIGPIPE"))
                return bus_set_transient_bool(u, name, &c->ignore_sigpipe, message, flags, error);

        if (streq(name, "TTYVHangup"))
                return bus_set_transient_bool(u, name, &c->tty_vhangup, message, flags, error);

        if (streq(name, "TTYReset"))
                return bus_set_transient_bool(u, name, &c->tty_reset, message, flags, error);

        if (streq(name, "TTYVTDisallocate"))
                return bus_set_transient_bool(u, name, &c->tty_vt_disallocate, message, flags, error);

        if (streq(name, "TTYRows"))
                return bus_set_transient_unsigned(u, name, &c->tty_rows, message, flags, error);

        if (streq(name, "TTYColumns"))
                return bus_set_transient_unsigned(u, name, &c->tty_cols, message, flags, error);

        if (streq(name, "PrivateTmp"))
                return bus_set_transient_bool(u, name, &c->private_tmp, message, flags, error);

        if (streq(name, "PrivateDevices"))
                return bus_set_transient_bool(u, name, &c->private_devices, message, flags, error);

        if (streq(name, "PrivateMounts"))
                return bus_set_transient_bool(u, name, &c->private_mounts, message, flags, error);

        if (streq(name, "PrivateNetwork"))
                return bus_set_transient_bool(u, name, &c->private_network, message, flags, error);

        if (streq(name, "PrivateIPC"))
                return bus_set_transient_bool(u, name, &c->private_ipc, message, flags, error);

        if (streq(name, "PrivateUsers"))
                return bus_set_transient_bool(u, name, &c->private_users, message, flags, error);

        if (streq(name, "NoNewPrivileges"))
                return bus_set_transient_bool(u, name, &c->no_new_privileges, message, flags, error);

        if (streq(name, "SyslogLevelPrefix"))
                return bus_set_transient_bool(u, name, &c->syslog_level_prefix, message, flags, error);

        if (streq(name, "MemoryDenyWriteExecute"))
                return bus_set_transient_bool(u, name, &c->memory_deny_write_execute, message, flags, error);

        if (streq(name, "RestrictRealtime"))
                return bus_set_transient_bool(u, name, &c->restrict_realtime, message, flags, error);

        if (streq(name, "RestrictSUIDSGID"))
                return bus_set_transient_bool(u, name, &c->restrict_suid_sgid, message, flags, error);

        if (streq(name, "DynamicUser"))
                return bus_set_transient_bool(u, name, &c->dynamic_user, message, flags, error);

        if (streq(name, "RemoveIPC"))
                return bus_set_transient_bool(u, name, &c->remove_ipc, message, flags, error);

        if (streq(name, "ProtectKernelTunables"))
                return bus_set_transient_bool(u, name, &c->protect_kernel_tunables, message, flags, error);

        if (streq(name, "ProtectKernelModules"))
                return bus_set_transient_bool(u, name, &c->protect_kernel_modules, message, flags, error);

        if (streq(name, "ProtectKernelLogs"))
                return bus_set_transient_bool(u, name, &c->protect_kernel_logs, message, flags, error);

        if (streq(name, "ProtectClock"))
                return bus_set_transient_bool(u, name, &c->protect_clock, message, flags, error);

        if (streq(name, "ProtectControlGroups"))
                return bus_set_transient_bool(u, name, &c->protect_control_groups, message, flags, error);

        if (streq(name, "CPUSchedulingResetOnFork"))
                return bus_set_transient_bool(u, name, &c->cpu_sched_reset_on_fork, message, flags, error);

        if (streq(name, "NonBlocking"))
                return bus_set_transient_bool(u, name, &c->non_blocking, message, flags, error);

        if (streq(name, "LockPersonality"))
                return bus_set_transient_bool(u, name, &c->lock_personality, message, flags, error);

        if (streq(name, "ProtectHostname"))
                return bus_set_transient_bool(u, name, &c->protect_hostname, message, flags, error);

        if (streq(name, "UtmpIdentifier"))
                return bus_set_transient_string(u, name, &c->utmp_id, message, flags, error);

        if (streq(name, "UtmpMode"))
                return bus_set_transient_utmp_mode(u, name, &c->utmp_mode, message, flags, error);

        if (streq(name, "PAMName"))
                return bus_set_transient_string(u, name, &c->pam_name, message, flags, error);

        if (streq(name, "TimerSlackNSec"))
                return bus_set_transient_nsec(u, name, &c->timer_slack_nsec, message, flags, error);

        if (streq(name, "ProtectSystem"))
                return bus_set_transient_protect_system(u, name, &c->protect_system, message, flags, error);

        if (streq(name, "ProtectHome"))
                return bus_set_transient_protect_home(u, name, &c->protect_home, message, flags, error);

        if (streq(name, "KeyringMode"))
                return bus_set_transient_keyring_mode(u, name, &c->keyring_mode, message, flags, error);

        if (streq(name, "ProtectProc"))
                return bus_set_transient_protect_proc(u, name, &c->protect_proc, message, flags, error);

        if (streq(name, "ProcSubset"))
                return bus_set_transient_proc_subset(u, name, &c->proc_subset, message, flags, error);

        if (streq(name, "RuntimeDirectoryPreserve"))
                return bus_set_transient_preserve_mode(u, name, &c->runtime_directory_preserve_mode, message, flags, error);

        if (streq(name, "UMask"))
                return bus_set_transient_mode_t(u, name, &c->umask, message, flags, error);

        if (streq(name, "RuntimeDirectoryMode"))
                return bus_set_transient_mode_t(u, name, &c->directories[EXEC_DIRECTORY_RUNTIME].mode, message, flags, error);

        if (streq(name, "StateDirectoryMode"))
                return bus_set_transient_mode_t(u, name, &c->directories[EXEC_DIRECTORY_STATE].mode, message, flags, error);

        if (streq(name, "CacheDirectoryMode"))
                return bus_set_transient_mode_t(u, name, &c->directories[EXEC_DIRECTORY_CACHE].mode, message, flags, error);

        if (streq(name, "LogsDirectoryMode"))
                return bus_set_transient_mode_t(u, name, &c->directories[EXEC_DIRECTORY_LOGS].mode, message, flags, error);

        if (streq(name, "ConfigurationDirectoryMode"))
                return bus_set_transient_mode_t(u, name, &c->directories[EXEC_DIRECTORY_CONFIGURATION].mode, message, flags, error);

        if (streq(name, "SELinuxContext"))
                return bus_set_transient_string(u, name, &c->selinux_context, message, flags, error);

        if (streq(name, "SecureBits"))
                return bus_set_transient_secure_bits(u, name, &c->secure_bits, message, flags, error);

        if (streq(name, "CapabilityBoundingSet"))
                return bus_set_transient_capability(u, name, &c->capability_bounding_set, message, flags, error);

        if (streq(name, "AmbientCapabilities"))
                return bus_set_transient_capability(u, name, &c->capability_ambient_set, message, flags, error);

        if (streq(name, "RestrictNamespaces"))
                return bus_set_transient_namespace_flag(u, name, &c->restrict_namespaces, message, flags, error);

        if (streq(name, "RestrictFileSystems")) {
                int allow_list;
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_enter_container(message, 'r', "bas");
                if (r < 0)
                        return r;

                r = sd_bus_message_read(message, "b", &allow_list);
                if (r < 0)
                        return r;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *joined = NULL;
                        FilesystemParseFlags invert_flag = allow_list ? 0 : FILESYSTEM_PARSE_INVERT;

                        if (strv_isempty(l)) {
                                c->restrict_filesystems_allow_list = false;
                                c->restrict_filesystems = set_free(c->restrict_filesystems);

                                unit_write_setting(u, flags, name, "RestrictFileSystems=");
                                return 1;
                        }

                        if (!c->restrict_filesystems)
                                c->restrict_filesystems_allow_list = allow_list;

                        STRV_FOREACH(s, l) {
                                r = lsm_bpf_parse_filesystem(
                                              *s,
                                              &c->restrict_filesystems,
                                              FILESYSTEM_PARSE_LOG|
                                              (invert_flag ? FILESYSTEM_PARSE_INVERT : 0)|
                                              (c->restrict_filesystems_allow_list ? FILESYSTEM_PARSE_ALLOW_LIST : 0),
                                              u->id, NULL, 0);
                                if (r < 0)
                                        return r;
                        }

                        joined = strv_join(l, " ");
                        if (!joined)
                                return -ENOMEM;

                        unit_write_settingf(u, flags, name, "%s=%s%s", name, allow_list ? "" : "~", joined);
                }

                return 1;
        }

        if (streq(name, "MountFlags"))
                return bus_set_transient_mount_flags(u, name, &c->mount_flags, message, flags, error);

        if (streq(name, "NetworkNamespacePath"))
                return bus_set_transient_path(u, name, &c->network_namespace_path, message, flags, error);

        if (streq(name, "IPCNamespacePath"))
                return bus_set_transient_path(u, name, &c->ipc_namespace_path, message, flags, error);

        if (streq(name, "SupplementaryGroups")) {
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l)
                        if (!isempty(*p) && !valid_user_group_name(*p, VALID_USER_ALLOW_NUMERIC|VALID_USER_RELAX|VALID_USER_WARN))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS,
                                                         "Invalid supplementary group names");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (strv_isempty(l)) {
                                c->supplementary_groups = strv_free(c->supplementary_groups);
                                unit_write_settingf(u, flags, name, "%s=", name);
                        } else {
                                _cleanup_free_ char *joined = NULL;

                                r = strv_extend_strv(&c->supplementary_groups, l, true);
                                if (r < 0)
                                        return -ENOMEM;

                                joined = strv_join(c->supplementary_groups, " ");
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "%s=%s", name, joined);
                        }
                }

                return 1;

        } else if (STR_IN_SET(name, "SetCredential", "SetCredentialEncrypted")) {
                bool isempty = true;

                r = sd_bus_message_enter_container(message, 'a', "(say)");
                if (r < 0)
                        return r;

                for (;;) {
                        const char *id;
                        const void *p;
                        size_t sz;

                        r = sd_bus_message_enter_container(message, 'r', "say");
                        if (r < 0)
                                return r;
                        if (r == 0)
                                break;

                        r = sd_bus_message_read(message, "s", &id);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_read_array(message, 'y', &p, &sz);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(message);
                        if (r < 0)
                                return r;

                        if (!credential_name_valid(id))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Credential ID is invalid: %s", id);

                        isempty = false;

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                _cleanup_free_ char *a = NULL, *b = NULL;
                                _cleanup_free_ void *copy = NULL;
                                ExecSetCredential *old;

                                copy = memdup(p, sz);
                                if (!copy)
                                        return -ENOMEM;

                                old = hashmap_get(c->set_credentials, id);
                                if (old) {
                                        free_and_replace(old->data, copy);
                                        old->size = sz;
                                        old->encrypted = streq(name, "SetCredentialEncrypted");
                                } else {
                                        _cleanup_(exec_set_credential_freep) ExecSetCredential *sc = NULL;

                                        sc = new(ExecSetCredential, 1);
                                        if (!sc)
                                                return -ENOMEM;

                                        *sc = (ExecSetCredential) {
                                                .id = strdup(id),
                                                .data = TAKE_PTR(copy),
                                                .size = sz,
                                                .encrypted = streq(name, "SetCredentialEncrypted"),
                                        };

                                        if (!sc->id)
                                                return -ENOMEM;

                                        r = hashmap_ensure_put(&c->set_credentials, &exec_set_credential_hash_ops, sc->id, sc);
                                        if (r < 0)
                                                return r;

                                        TAKE_PTR(sc);
                                }

                                a = specifier_escape(id);
                                if (!a)
                                        return -ENOMEM;

                                b = cescape_length(p, sz);
                                if (!b)
                                        return -ENOMEM;

                                (void) unit_write_settingf(u, flags, name, "%s=%s:%s", name, a, b);
                        }
                }

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags) && isempty) {
                        c->set_credentials = hashmap_free(c->set_credentials);
                        (void) unit_write_settingf(u, flags, name, "%s=", name);
                }

                return 1;

        } else if (STR_IN_SET(name, "LoadCredential", "LoadCredentialEncrypted")) {
                bool isempty = true;

                r = sd_bus_message_enter_container(message, 'a', "(ss)");
                if (r < 0)
                        return r;

                for (;;) {
                        const char *id, *source;

                        r = sd_bus_message_read(message, "(ss)", &id, &source);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                break;

                        if (!credential_name_valid(id))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Credential ID is invalid: %s", id);

                        if (!(path_is_absolute(source) ? path_is_normalized(source) : credential_name_valid(source)))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Credential source is invalid: %s", source);

                        isempty = false;

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                _cleanup_free_ char *copy = NULL;
                                ExecLoadCredential *old;

                                copy = strdup(source);
                                if (!copy)
                                        return -ENOMEM;

                                old = hashmap_get(c->load_credentials, id);
                                if (old) {
                                        free_and_replace(old->path, copy);
                                        old->encrypted = streq(name, "LoadCredentialEncrypted");
                                } else {
                                        _cleanup_(exec_load_credential_freep) ExecLoadCredential *lc = NULL;

                                        lc = new(ExecLoadCredential, 1);
                                        if (!lc)
                                                return -ENOMEM;

                                        *lc = (ExecLoadCredential) {
                                                .id = strdup(id),
                                                .path = TAKE_PTR(copy),
                                                .encrypted = streq(name, "LoadCredentialEncrypted"),
                                        };

                                        if (!lc->id)
                                                return -ENOMEM;

                                        r = hashmap_ensure_put(&c->load_credentials, &exec_load_credential_hash_ops, lc->id, lc);
                                        if (r < 0)
                                                return r;

                                        TAKE_PTR(lc);
                                }

                                (void) unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "%s=%s:%s", name, id, source);
                        }
                }

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags) && isempty) {
                        c->load_credentials = hashmap_free(c->load_credentials);
                        (void) unit_write_settingf(u, flags, name, "%s=", name);
                }

                return 1;

        } else if (streq(name, "SyslogLevel")) {
                int32_t level;

                r = sd_bus_message_read(message, "i", &level);
                if (r < 0)
                        return r;

                if (!log_level_is_valid(level))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Log level value out of range");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        c->syslog_priority = (c->syslog_priority & LOG_FACMASK) | level;
                        unit_write_settingf(u, flags, name, "SyslogLevel=%i", level);
                }

                return 1;

        } else if (streq(name, "SyslogFacility")) {
                int32_t facility;

                r = sd_bus_message_read(message, "i", &facility);
                if (r < 0)
                        return r;

                if (!log_facility_unshifted_is_valid(facility))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Log facility value out of range");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        c->syslog_priority = (facility << 3) | LOG_PRI(c->syslog_priority);
                        unit_write_settingf(u, flags, name, "SyslogFacility=%i", facility);
                }

                return 1;

        } else if (streq(name, "LogNamespace")) {
                const char *n;

                r = sd_bus_message_read(message, "s", &n);
                if (r < 0)
                        return r;

                if (!isempty(n) && !log_namespace_name_valid(n))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Log namespace name not valid");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {

                        if (isempty(n)) {
                                c->log_namespace = mfree(c->log_namespace);
                                unit_write_settingf(u, flags, name, "%s=", name);
                        } else {
                                r = free_and_strdup(&c->log_namespace, n);
                                if (r < 0)
                                        return r;

                                unit_write_settingf(u, flags, name, "%s=%s", name, n);
                        }
                }

                return 1;

        } else if (streq(name, "LogExtraFields")) {
                size_t n = 0;

                r = sd_bus_message_enter_container(message, 'a', "ay");
                if (r < 0)
                        return r;

                for (;;) {
                        _cleanup_free_ void *copy = NULL;
                        struct iovec *t;
                        const char *eq;
                        const void *p;
                        size_t sz;

                        /* Note that we expect a byte array for each field, instead of a string. That's because on the
                         * lower-level journal fields can actually contain binary data and are not restricted to text,
                         * and we should not "lose precision" in our types on the way. That said, I am pretty sure
                         * actually encoding binary data as unit metadata is not a good idea. Hence we actually refuse
                         * any actual binary data, and only accept UTF-8. This allows us to eventually lift this
                         * limitation, should a good, valid usecase arise. */

                        r = sd_bus_message_read_array(message, 'y', &p, &sz);
                        if (r < 0)
                                return r;
                        if (r == 0)
                                break;

                        if (memchr(p, 0, sz))
                                return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Journal field contains zero byte");

                        eq = memchr(p, '=', sz);
                        if (!eq)
                                return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Journal field contains no '=' character");
                        if (!journal_field_valid(p, eq - (const char*) p, false))
                                return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Journal field invalid");

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                t = reallocarray(c->log_extra_fields, c->n_log_extra_fields+1, sizeof(struct iovec));
                                if (!t)
                                        return -ENOMEM;
                                c->log_extra_fields = t;
                        }

                        copy = malloc(sz + 1);
                        if (!copy)
                                return -ENOMEM;

                        memcpy(copy, p, sz);
                        ((uint8_t*) copy)[sz] = 0;

                        if (!utf8_is_valid(copy))
                                return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Journal field is not valid UTF-8");

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                c->log_extra_fields[c->n_log_extra_fields++] = IOVEC_MAKE(copy, sz);
                                unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS|UNIT_ESCAPE_C, name, "LogExtraFields=%s", (char*) copy);

                                copy = NULL;
                        }

                        n++;
                }

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags) && n == 0) {
                        exec_context_free_log_extra_fields(c);
                        unit_write_setting(u, flags, name, "LogExtraFields=");
                }

                return 1;
        }

#if HAVE_SECCOMP

        if (streq(name, "SystemCallErrorNumber"))
                return bus_set_transient_errno(u, name, &c->syscall_errno, message, flags, error);

        if (streq(name, "SystemCallFilter")) {
                int allow_list;
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_enter_container(message, 'r', "bas");
                if (r < 0)
                        return r;

                r = sd_bus_message_read(message, "b", &allow_list);
                if (r < 0)
                        return r;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *joined = NULL;
                        SeccompParseFlags invert_flag = allow_list ? 0 : SECCOMP_PARSE_INVERT;

                        if (strv_isempty(l)) {
                                c->syscall_allow_list = false;
                                c->syscall_filter = hashmap_free(c->syscall_filter);

                                unit_write_settingf(u, flags, name, "SystemCallFilter=");
                                return 1;
                        }

                        if (!c->syscall_filter) {
                                c->syscall_filter = hashmap_new(NULL);
                                if (!c->syscall_filter)
                                        return log_oom();

                                c->syscall_allow_list = allow_list;

                                if (c->syscall_allow_list) {
                                        r = seccomp_parse_syscall_filter("@default",
                                                                         -1,
                                                                         c->syscall_filter,
                                                                         SECCOMP_PARSE_PERMISSIVE |
                                                                         SECCOMP_PARSE_ALLOW_LIST,
                                                                         u->id,
                                                                         NULL, 0);
                                        if (r < 0)
                                                return r;
                                }
                        }

                        STRV_FOREACH(s, l) {
                                _cleanup_free_ char *n = NULL;
                                int e;

                                r = parse_syscall_and_errno(*s, &n, &e);
                                if (r < 0)
                                        return r;

                                if (allow_list && e >= 0)
                                        return -EINVAL;

                                r = seccomp_parse_syscall_filter(n,
                                                                 e,
                                                                 c->syscall_filter,
                                                                 SECCOMP_PARSE_LOG | SECCOMP_PARSE_PERMISSIVE |
                                                                 invert_flag |
                                                                 (c->syscall_allow_list ? SECCOMP_PARSE_ALLOW_LIST : 0),
                                                                 u->id,
                                                                 NULL, 0);
                                if (r < 0)
                                        return r;
                        }

                        joined = strv_join(l, " ");
                        if (!joined)
                                return -ENOMEM;

                        unit_write_settingf(u, flags, name, "SystemCallFilter=%s%s", allow_list ? "" : "~", joined);
                }

                return 1;

        } else if (streq(name, "SystemCallLog")) {
                int allow_list;
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_enter_container(message, 'r', "bas");
                if (r < 0)
                        return r;

                r = sd_bus_message_read(message, "b", &allow_list);
                if (r < 0)
                        return r;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *joined = NULL;
                        SeccompParseFlags invert_flag = allow_list ? 0 : SECCOMP_PARSE_INVERT;

                        if (strv_isempty(l)) {
                                c->syscall_log_allow_list = false;
                                c->syscall_log = hashmap_free(c->syscall_log);

                                unit_write_settingf(u, flags, name, "SystemCallLog=");
                                return 1;
                        }

                        if (!c->syscall_log) {
                                c->syscall_log = hashmap_new(NULL);
                                if (!c->syscall_log)
                                        return log_oom();

                                c->syscall_log_allow_list = allow_list;
                        }

                        STRV_FOREACH(s, l) {
                                r = seccomp_parse_syscall_filter(*s,
                                                                 -1, /* errno not used */
                                                                 c->syscall_log,
                                                                 SECCOMP_PARSE_LOG | SECCOMP_PARSE_PERMISSIVE |
                                                                 invert_flag |
                                                                 (c->syscall_log_allow_list ? SECCOMP_PARSE_ALLOW_LIST : 0),
                                                                 u->id,
                                                                 NULL, 0);
                                if (r < 0)
                                        return r;
                        }

                        joined = strv_join(l, " ");
                        if (!joined)
                                return -ENOMEM;

                        unit_write_settingf(u, flags, name, "SystemCallLog=%s%s", allow_list ? "" : "~", joined);
                }

                return 1;

        } else if (streq(name, "SystemCallArchitectures")) {
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *joined = NULL;

                        if (strv_isempty(l))
                                c->syscall_archs = set_free(c->syscall_archs);
                        else
                                STRV_FOREACH(s, l) {
                                        uint32_t a;

                                        r = seccomp_arch_from_string(*s, &a);
                                        if (r < 0)
                                                return r;

                                        r = set_ensure_put(&c->syscall_archs, NULL, UINT32_TO_PTR(a + 1));
                                        if (r < 0)
                                                return r;
                                }

                        joined = strv_join(l, " ");
                        if (!joined)
                                return -ENOMEM;

                        unit_write_settingf(u, flags, name, "%s=%s", name, joined);
                }

                return 1;

        } else if (streq(name, "RestrictAddressFamilies")) {
                _cleanup_strv_free_ char **l = NULL;
                int allow_list;

                r = sd_bus_message_enter_container(message, 'r', "bas");
                if (r < 0)
                        return r;

                r = sd_bus_message_read(message, "b", &allow_list);
                if (r < 0)
                        return r;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *joined = NULL;

                        if (strv_isempty(l)) {
                                c->address_families_allow_list = allow_list;
                                c->address_families = set_free(c->address_families);

                                unit_write_settingf(u, flags, name, "RestrictAddressFamilies=%s",
                                                    allow_list ? "none" : "");
                                return 1;
                        }

                        if (!c->address_families) {
                                c->address_families = set_new(NULL);
                                if (!c->address_families)
                                        return log_oom();

                                c->address_families_allow_list = allow_list;
                        }

                        STRV_FOREACH(s, l) {
                                int af;

                                af = af_from_name(*s);
                                if (af < 0)
                                        return af;

                                if (allow_list == c->address_families_allow_list) {
                                        r = set_put(c->address_families, INT_TO_PTR(af));
                                        if (r < 0)
                                                return r;
                                } else
                                        set_remove(c->address_families, INT_TO_PTR(af));
                        }

                        joined = strv_join(l, " ");
                        if (!joined)
                                return -ENOMEM;

                        unit_write_settingf(u, flags, name, "RestrictAddressFamilies=%s%s", allow_list ? "" : "~", joined);
                }

                return 1;
        }
#endif
        if (STR_IN_SET(name, "CPUAffinity", "NUMAMask")) {
                const void *a;
                size_t n;
                bool affinity = streq(name, "CPUAffinity");
                _cleanup_(cpu_set_reset) CPUSet set = {};

                r = sd_bus_message_read_array(message, 'y', &a, &n);
                if (r < 0)
                        return r;

                r = cpu_set_from_dbus(a, n, &set);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (n == 0) {
                                cpu_set_reset(affinity ? &c->cpu_set : &c->numa_policy.nodes);
                                unit_write_settingf(u, flags, name, "%s=", name);
                        } else {
                                _cleanup_free_ char *str = NULL;

                                str = cpu_set_to_string(&set);
                                if (!str)
                                        return -ENOMEM;

                                /* We forego any optimizations here, and always create the structure using
                                 * cpu_set_add_all(), because we don't want to care if the existing size we
                                 * got over dbus is appropriate. */
                                r = cpu_set_add_all(affinity ? &c->cpu_set : &c->numa_policy.nodes, &set);
                                if (r < 0)
                                        return r;

                                unit_write_settingf(u, flags, name, "%s=%s", name, str);
                        }
                }

                return 1;

        } else if (streq(name, "CPUAffinityFromNUMA")) {
                int q;

                r = sd_bus_message_read_basic(message, 'b', &q);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        c->cpu_affinity_from_numa = q;
                        unit_write_settingf(u, flags, name, "%s=%s", "CPUAffinity", "numa");
                }

                return 1;

        } else if (streq(name, "NUMAPolicy")) {
                int32_t type;

                r = sd_bus_message_read(message, "i", &type);
                if (r < 0)
                        return r;

                if (!mpol_is_valid(type))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid NUMAPolicy value: %i", type);

                if (!UNIT_WRITE_FLAGS_NOOP(flags))
                        c->numa_policy.type = type;

                return 1;

        } else if (streq(name, "Nice")) {
                int32_t q;

                r = sd_bus_message_read(message, "i", &q);
                if (r < 0)
                        return r;

                if (!nice_is_valid(q))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid Nice value: %i", q);

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        c->nice = q;
                        c->nice_set = true;

                        unit_write_settingf(u, flags, name, "Nice=%i", q);
                }

                return 1;

        } else if (streq(name, "CPUSchedulingPolicy")) {
                int32_t q;

                r = sd_bus_message_read(message, "i", &q);
                if (r < 0)
                        return r;

                if (!sched_policy_is_valid(q))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid CPU scheduling policy: %i", q);

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *s = NULL;

                        r = sched_policy_to_string_alloc(q, &s);
                        if (r < 0)
                                return r;

                        c->cpu_sched_policy = q;
                        c->cpu_sched_priority = CLAMP(c->cpu_sched_priority, sched_get_priority_min(q), sched_get_priority_max(q));
                        c->cpu_sched_set = true;

                        unit_write_settingf(u, flags, name, "CPUSchedulingPolicy=%s", s);
                }

                return 1;

        } else if (streq(name, "CPUSchedulingPriority")) {
                int32_t p, min, max;

                r = sd_bus_message_read(message, "i", &p);
                if (r < 0)
                        return r;

                min = sched_get_priority_min(c->cpu_sched_policy);
                max = sched_get_priority_max(c->cpu_sched_policy);
                if (p < min || p > max)
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid CPU scheduling priority: %i", p);

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        c->cpu_sched_priority = p;
                        c->cpu_sched_set = true;

                        unit_write_settingf(u, flags, name, "CPUSchedulingPriority=%i", p);
                }

                return 1;

        } else if (streq(name, "IOSchedulingClass")) {
                int32_t q;

                r = sd_bus_message_read(message, "i", &q);
                if (r < 0)
                        return r;

                if (!ioprio_class_is_valid(q))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid IO scheduling class: %i", q);

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *s = NULL;

                        r = ioprio_class_to_string_alloc(q, &s);
                        if (r < 0)
                                return r;

                        c->ioprio = ioprio_normalize(ioprio_prio_value(q, ioprio_prio_data(c->ioprio)));
                        c->ioprio_set = true;

                        unit_write_settingf(u, flags, name, "IOSchedulingClass=%s", s);
                }

                return 1;

        } else if (streq(name, "IOSchedulingPriority")) {
                int32_t p;

                r = sd_bus_message_read(message, "i", &p);
                if (r < 0)
                        return r;

                if (!ioprio_priority_is_valid(p))
                        return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid IO scheduling priority: %i", p);

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        c->ioprio = ioprio_normalize(ioprio_prio_value(ioprio_prio_class(c->ioprio), p));
                        c->ioprio_set = true;

                        unit_write_settingf(u, flags, name, "IOSchedulingPriority=%i", p);
                }

                return 1;

        } else if (streq(name, "MountAPIVFS")) {
                bool b;

                r = bus_set_transient_bool(u, name, &b, message, flags, error);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        c->mount_apivfs = b;
                        c->mount_apivfs_set = true;
                }

                return 1;

        } else if (streq(name, "WorkingDirectory")) {
                const char *s;
                bool missing_ok;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                if (s[0] == '-') {
                        missing_ok = true;
                        s++;
                } else
                        missing_ok = false;

                if (!isempty(s) && !streq(s, "~") && !path_is_absolute(s))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "WorkingDirectory= expects an absolute path or '~'");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (streq(s, "~")) {
                                c->working_directory = mfree(c->working_directory);
                                c->working_directory_home = true;
                        } else {
                                r = free_and_strdup(&c->working_directory, empty_to_null(s));
                                if (r < 0)
                                        return r;

                                c->working_directory_home = false;
                        }

                        c->working_directory_missing_ok = missing_ok;
                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "WorkingDirectory=%s%s", missing_ok ? "-" : "", s);
                }

                return 1;

        } else if (STR_IN_SET(name,
                              "StandardInputFileDescriptorName", "StandardOutputFileDescriptorName", "StandardErrorFileDescriptorName")) {
                const char *s;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                if (!isempty(s) && !fdname_is_valid(s))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid file descriptor name");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {

                        if (streq(name, "StandardInputFileDescriptorName")) {
                                r = free_and_strdup(c->stdio_fdname + STDIN_FILENO, empty_to_null(s));
                                if (r < 0)
                                        return r;

                                c->std_input = EXEC_INPUT_NAMED_FD;
                                unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardInput=fd:%s", exec_context_fdname(c, STDIN_FILENO));

                        } else if (streq(name, "StandardOutputFileDescriptorName")) {
                                r = free_and_strdup(c->stdio_fdname + STDOUT_FILENO, empty_to_null(s));
                                if (r < 0)
                                        return r;

                                c->std_output = EXEC_OUTPUT_NAMED_FD;
                                unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardOutput=fd:%s", exec_context_fdname(c, STDOUT_FILENO));

                        } else {
                                assert(streq(name, "StandardErrorFileDescriptorName"));

                                r = free_and_strdup(&c->stdio_fdname[STDERR_FILENO], empty_to_null(s));
                                if (r < 0)
                                        return r;

                                c->std_error = EXEC_OUTPUT_NAMED_FD;
                                unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardError=fd:%s", exec_context_fdname(c, STDERR_FILENO));
                        }
                }

                return 1;

        } else if (STR_IN_SET(name,
                              "StandardInputFile",
                              "StandardOutputFile", "StandardOutputFileToAppend", "StandardOutputFileToTruncate",
                              "StandardErrorFile", "StandardErrorFileToAppend", "StandardErrorFileToTruncate")) {
                const char *s;

                r = sd_bus_message_read(message, "s", &s);
                if (r < 0)
                        return r;

                if (!isempty(s)) {
                        if (!path_is_absolute(s))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Path %s is not absolute", s);
                        if (!path_is_normalized(s))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Path %s is not normalized", s);
                }

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {

                        if (streq(name, "StandardInputFile")) {
                                r = free_and_strdup(&c->stdio_file[STDIN_FILENO], empty_to_null(s));
                                if (r < 0)
                                        return r;

                                c->std_input = EXEC_INPUT_FILE;
                                unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardInput=file:%s", s);

                        } else if (STR_IN_SET(name, "StandardOutputFile", "StandardOutputFileToAppend", "StandardOutputFileToTruncate")) {
                                r = free_and_strdup(&c->stdio_file[STDOUT_FILENO], empty_to_null(s));
                                if (r < 0)
                                        return r;

                                if (streq(name, "StandardOutputFile")) {
                                        c->std_output = EXEC_OUTPUT_FILE;
                                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardOutput=file:%s", s);
                                } else if (streq(name, "StandardOutputFileToAppend")) {
                                        c->std_output = EXEC_OUTPUT_FILE_APPEND;
                                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardOutput=append:%s", s);
                                } else {
                                        assert(streq(name, "StandardOutputFileToTruncate"));
                                        c->std_output = EXEC_OUTPUT_FILE_TRUNCATE;
                                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardOutput=truncate:%s", s);
                                }
                        } else {
                                assert(STR_IN_SET(name, "StandardErrorFile", "StandardErrorFileToAppend", "StandardErrorFileToTruncate"));

                                r = free_and_strdup(&c->stdio_file[STDERR_FILENO], empty_to_null(s));
                                if (r < 0)
                                        return r;

                                if (streq(name, "StandardErrorFile")) {
                                        c->std_error = EXEC_OUTPUT_FILE;
                                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardError=file:%s", s);
                                } else if (streq(name, "StandardErrorFileToAppend")) {
                                        c->std_error = EXEC_OUTPUT_FILE_APPEND;
                                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardError=append:%s", s);
                                } else {
                                        assert(streq(name, "StandardErrorFileToTruncate"));
                                        c->std_error = EXEC_OUTPUT_FILE_TRUNCATE;
                                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "StandardError=truncate:%s", s);
                                }
                        }
                }

                return 1;

        } else if (streq(name, "StandardInputData")) {
                const void *p;
                size_t sz;

                r = sd_bus_message_read_array(message, 'y', &p, &sz);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        _cleanup_free_ char *encoded = NULL;

                        if (sz == 0) {
                                c->stdin_data = mfree(c->stdin_data);
                                c->stdin_data_size = 0;

                                unit_write_settingf(u, flags, name, "StandardInputData=");
                        } else {
                                void *q;
                                ssize_t n;

                                if (c->stdin_data_size + sz < c->stdin_data_size || /* check for overflow */
                                    c->stdin_data_size + sz > EXEC_STDIN_DATA_MAX)
                                        return -E2BIG;

                                n = base64mem(p, sz, &encoded);
                                if (n < 0)
                                        return (int) n;

                                q = realloc(c->stdin_data, c->stdin_data_size + sz);
                                if (!q)
                                        return -ENOMEM;

                                memcpy((uint8_t*) q + c->stdin_data_size, p, sz);

                                c->stdin_data = q;
                                c->stdin_data_size += sz;

                                unit_write_settingf(u, flags, name, "StandardInputData=%s", encoded);
                        }
                }

                return 1;

        } else if (streq(name, "Environment")) {

                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                if (!strv_env_is_valid(l))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid environment block.");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (strv_isempty(l)) {
                                c->environment = strv_free(c->environment);
                                unit_write_setting(u, flags, name, "Environment=");
                        } else {
                                _cleanup_free_ char *joined = NULL;
                                char **e;

                                joined = unit_concat_strv(l, UNIT_ESCAPE_SPECIFIERS|UNIT_ESCAPE_C);
                                if (!joined)
                                        return -ENOMEM;

                                e = strv_env_merge(c->environment, l);
                                if (!e)
                                        return -ENOMEM;

                                strv_free_and_replace(c->environment, e);
                                unit_write_settingf(u, flags, name, "Environment=%s", joined);
                        }
                }

                return 1;

        } else if (streq(name, "UnsetEnvironment")) {

                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                if (!strv_env_name_or_assignment_is_valid(l))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid UnsetEnvironment= list.");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (strv_isempty(l)) {
                                c->unset_environment = strv_free(c->unset_environment);
                                unit_write_setting(u, flags, name, "UnsetEnvironment=");
                        } else {
                                _cleanup_free_ char *joined = NULL;
                                char **e;

                                joined = unit_concat_strv(l, UNIT_ESCAPE_SPECIFIERS|UNIT_ESCAPE_C);
                                if (!joined)
                                        return -ENOMEM;

                                e = strv_env_merge(c->unset_environment, l);
                                if (!e)
                                        return -ENOMEM;

                                strv_free_and_replace(c->unset_environment, e);
                                unit_write_settingf(u, flags, name, "UnsetEnvironment=%s", joined);
                        }
                }

                return 1;

        } else if (streq(name, "OOMScoreAdjust")) {
                int oa;

                r = sd_bus_message_read(message, "i", &oa);
                if (r < 0)
                        return r;

                if (!oom_score_adjust_is_valid(oa))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "OOM score adjust value out of range");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        c->oom_score_adjust = oa;
                        c->oom_score_adjust_set = true;
                        unit_write_settingf(u, flags, name, "OOMScoreAdjust=%i", oa);
                }

                return 1;

        } else if (streq(name, "CoredumpFilter")) {
                uint64_t f;

                r = sd_bus_message_read(message, "t", &f);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        c->coredump_filter = f;
                        c->coredump_filter_set = true;
                        unit_write_settingf(u, flags, name, "CoredumpFilter=0x%"PRIx64, f);
                }

                return 1;

        } else if (streq(name, "EnvironmentFiles")) {

                _cleanup_free_ char *joined = NULL;
                _cleanup_fclose_ FILE *f = NULL;
                _cleanup_strv_free_ char **l = NULL;
                size_t size = 0;

                r = sd_bus_message_enter_container(message, 'a', "(sb)");
                if (r < 0)
                        return r;

                f = open_memstream_unlocked(&joined, &size);
                if (!f)
                        return -ENOMEM;

                fputs("EnvironmentFile=\n", f);

                STRV_FOREACH(i, c->environment_files) {
                        _cleanup_free_ char *q = NULL;

                        q = specifier_escape(*i);
                        if (!q)
                                return -ENOMEM;

                        fprintf(f, "EnvironmentFile=%s\n", q);
                }

                while ((r = sd_bus_message_enter_container(message, 'r', "sb")) > 0) {
                        const char *path;
                        int b;

                        r = sd_bus_message_read(message, "sb", &path, &b);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(message);
                        if (r < 0)
                                return r;

                        if (!path_is_absolute(path))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Path %s is not absolute.", path);

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                _cleanup_free_ char *q = NULL, *buf = NULL;

                                buf = strjoin(b ? "-" : "", path);
                                if (!buf)
                                        return -ENOMEM;

                                q = specifier_escape(buf);
                                if (!q)
                                        return -ENOMEM;

                                fprintf(f, "EnvironmentFile=%s\n", q);

                                r = strv_consume(&l, TAKE_PTR(buf));
                                if (r < 0)
                                        return r;
                        }
                }
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                r = fflush_and_check(f);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (strv_isempty(l)) {
                                c->environment_files = strv_free(c->environment_files);
                                unit_write_setting(u, flags, name, "EnvironmentFile=");
                        } else {
                                r = strv_extend_strv(&c->environment_files, l, true);
                                if (r < 0)
                                        return r;

                                unit_write_setting(u, flags, name, joined);
                        }
                }

                return 1;

        } else if (streq(name, "PassEnvironment")) {

                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                if (!strv_env_name_is_valid(l))
                        return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid PassEnvironment= block.");

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (strv_isempty(l)) {
                                c->pass_environment = strv_free(c->pass_environment);
                                unit_write_setting(u, flags, name, "PassEnvironment=");
                        } else {
                                _cleanup_free_ char *joined = NULL;

                                r = strv_extend_strv(&c->pass_environment, l, true);
                                if (r < 0)
                                        return r;

                                /* We write just the new settings out to file, with unresolved specifiers. */
                                joined = unit_concat_strv(l, UNIT_ESCAPE_SPECIFIERS);
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_settingf(u, flags, name, "PassEnvironment=%s", joined);
                        }
                }

                return 1;

        } else if (STR_IN_SET(name, "ReadWriteDirectories", "ReadOnlyDirectories", "InaccessibleDirectories",
                              "ReadWritePaths", "ReadOnlyPaths", "InaccessiblePaths", "ExecPaths", "NoExecPaths",
                              "ExtensionDirectories")) {
                _cleanup_strv_free_ char **l = NULL;
                char ***dirs;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l) {
                        char *i = *p;
                        size_t offset;

                        offset = i[0] == '-';
                        offset += i[offset] == '+';
                        if (!path_is_absolute(i + offset))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid %s", name);

                        path_simplify(i + offset);
                }

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (STR_IN_SET(name, "ReadWriteDirectories", "ReadWritePaths"))
                                dirs = &c->read_write_paths;
                        else if (STR_IN_SET(name, "ReadOnlyDirectories", "ReadOnlyPaths"))
                                dirs = &c->read_only_paths;
                        else if (streq(name, "ExecPaths"))
                                dirs = &c->exec_paths;
                        else if (streq(name, "NoExecPaths"))
                                dirs = &c->no_exec_paths;
                        else if (streq(name, "ExtensionDirectories"))
                                dirs = &c->extension_directories;
                        else /* "InaccessiblePaths" */
                                dirs = &c->inaccessible_paths;

                        if (strv_isempty(l)) {
                                *dirs = strv_free(*dirs);
                                unit_write_settingf(u, flags, name, "%s=", name);
                        } else {
                                _cleanup_free_ char *joined = NULL;

                                joined = unit_concat_strv(l, UNIT_ESCAPE_SPECIFIERS);
                                if (!joined)
                                        return -ENOMEM;

                                r = strv_extend_strv(dirs, l, true);
                                if (r < 0)
                                        return -ENOMEM;

                                unit_write_settingf(u, flags, name, "%s=%s", name, joined);
                        }
                }

                return 1;

        } else if (streq(name, "ExecSearchPath")) {
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l)
                        if (!path_is_absolute(*p) || !path_is_normalized(*p) || strchr(*p, ':'))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Invalid %s", name);

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (strv_isempty(l)) {
                                c->exec_search_path = strv_free(c->exec_search_path);
                                unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "ExecSearchPath=");
                        } else {
                                _cleanup_free_ char *joined = NULL;
                                r = strv_extend_strv(&c->exec_search_path, l, true);
                                if (r < 0)
                                        return -ENOMEM;
                                joined = strv_join(c->exec_search_path, ":");
                                if (!joined)
                                        return log_oom();
                                unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "ExecSearchPath=%s", joined);
                        }
                }

                return 1;

        } else if (STR_IN_SET(name, "RuntimeDirectory", "StateDirectory", "CacheDirectory", "LogsDirectory", "ConfigurationDirectory")) {
                _cleanup_strv_free_ char **l = NULL;

                r = sd_bus_message_read_strv(message, &l);
                if (r < 0)
                        return r;

                STRV_FOREACH(p, l) {
                        if (!path_is_normalized(*p))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s= path is not normalized: %s", name, *p);

                        if (path_is_absolute(*p))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s= path is absolute: %s", name, *p);

                        if (path_startswith(*p, "private"))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "%s= path can't be 'private': %s", name, *p);
                }

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        ExecDirectoryType i;
                        ExecDirectory *d;

                        assert_se((i = exec_directory_type_from_string(name)) >= 0);
                        d = c->directories + i;

                        if (strv_isempty(l)) {
                                exec_directory_done(d);
                                unit_write_settingf(u, flags, name, "%s=", name);
                        } else {
                                _cleanup_free_ char *joined = NULL;

                                STRV_FOREACH(source, l) {
                                        r = exec_directory_add(&d->items, &d->n_items, *source, NULL);
                                        if (r < 0)
                                                return log_oom();
                                }

                                joined = unit_concat_strv(l, UNIT_ESCAPE_SPECIFIERS);
                                if (!joined)
                                        return -ENOMEM;

                                unit_write_settingf(u, flags, name, "%s=%s", name, joined);
                        }
                }

                return 1;

        } else if (STR_IN_SET(name, "AppArmorProfile", "SmackProcessLabel")) {
                int ignore;
                const char *s;

                r = sd_bus_message_read(message, "(bs)", &ignore, &s);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        char **p;
                        bool *b;

                        if (streq(name, "AppArmorProfile")) {
                                p = &c->apparmor_profile;
                                b = &c->apparmor_profile_ignore;
                        } else { /* "SmackProcessLabel" */
                                p = &c->smack_process_label;
                                b = &c->smack_process_label_ignore;
                        }

                        if (isempty(s)) {
                                *p = mfree(*p);
                                *b = false;
                        } else {
                                if (free_and_strdup(p, s) < 0)
                                        return -ENOMEM;
                                *b = ignore;
                        }

                        unit_write_settingf(u, flags|UNIT_ESCAPE_SPECIFIERS, name, "%s=%s%s", name, ignore ? "-" : "", strempty(s));
                }

                return 1;

        } else if (STR_IN_SET(name, "BindPaths", "BindReadOnlyPaths")) {
                char *source, *destination;
                int ignore_enoent;
                uint64_t mount_flags;
                bool empty = true;

                r = sd_bus_message_enter_container(message, 'a', "(ssbt)");
                if (r < 0)
                        return r;

                while ((r = sd_bus_message_read(message, "(ssbt)", &source, &destination, &ignore_enoent, &mount_flags)) > 0) {

                        if (!path_is_absolute(source))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Source path %s is not absolute.", source);
                        if (!path_is_absolute(destination))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Destination path %s is not absolute.", destination);
                        if (!IN_SET(mount_flags, 0, MS_REC))
                                return sd_bus_error_set(error, SD_BUS_ERROR_INVALID_ARGS, "Unknown mount flags.");

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                r = bind_mount_add(&c->bind_mounts, &c->n_bind_mounts,
                                                   &(BindMount) {
                                                           .source = source,
                                                           .destination = destination,
                                                           .read_only = !!strstr(name, "ReadOnly"),
                                                           .recursive = !!(mount_flags & MS_REC),
                                                           .ignore_enoent = ignore_enoent,
                                                   });
                                if (r < 0)
                                        return r;

                                unit_write_settingf(
                                                u, flags|UNIT_ESCAPE_SPECIFIERS, name,
                                                "%s=%s%s:%s:%s",
                                                name,
                                                ignore_enoent ? "-" : "",
                                                source,
                                                destination,
                                                (mount_flags & MS_REC) ? "rbind" : "norbind");
                        }

                        empty = false;
                }
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (empty) {
                        bind_mount_free_many(c->bind_mounts, c->n_bind_mounts);
                        c->bind_mounts = NULL;
                        c->n_bind_mounts = 0;

                        unit_write_settingf(u, flags, name, "%s=", name);
                }

                return 1;

        } else if (streq(name, "TemporaryFileSystem")) {
                const char *path, *options;
                bool empty = true;

                r = sd_bus_message_enter_container(message, 'a', "(ss)");
                if (r < 0)
                        return r;

                while ((r = sd_bus_message_read(message, "(ss)", &path, &options)) > 0) {

                        if (!path_is_absolute(path))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Mount point %s is not absolute.", path);

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                r = temporary_filesystem_add(&c->temporary_filesystems, &c->n_temporary_filesystems, path, options);
                                if (r < 0)
                                        return r;

                                unit_write_settingf(
                                                u, flags|UNIT_ESCAPE_SPECIFIERS, name,
                                                "%s=%s:%s",
                                                name,
                                                path,
                                                options);
                        }

                        empty = false;
                }
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (empty) {
                        temporary_filesystem_free_many(c->temporary_filesystems, c->n_temporary_filesystems);
                        c->temporary_filesystems = NULL;
                        c->n_temporary_filesystems = 0;

                        unit_write_settingf(u, flags, name, "%s=", name);
                }

                return 1;

        } else if ((suffix = startswith(name, "Limit"))) {
                const char *soft = NULL;
                int ri;

                ri = rlimit_from_string(suffix);
                if (ri < 0) {
                        soft = endswith(suffix, "Soft");
                        if (soft) {
                                const char *n;

                                n = strndupa_safe(suffix, soft - suffix);
                                ri = rlimit_from_string(n);
                                if (ri >= 0)
                                        name = strjoina("Limit", n);
                        }
                }

                if (ri >= 0) {
                        uint64_t rl;
                        rlim_t x;

                        r = sd_bus_message_read(message, "t", &rl);
                        if (r < 0)
                                return r;

                        if (rl == UINT64_MAX)
                                x = RLIM_INFINITY;
                        else {
                                x = (rlim_t) rl;

                                if ((uint64_t) x != rl)
                                        return -ERANGE;
                        }

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                _cleanup_free_ char *f = NULL;
                                struct rlimit nl;

                                if (c->rlimit[ri]) {
                                        nl = *c->rlimit[ri];

                                        if (soft)
                                                nl.rlim_cur = x;
                                        else
                                                nl.rlim_max = x;
                                } else
                                        /* When the resource limit is not initialized yet, then assign the value to both fields */
                                        nl = (struct rlimit) {
                                                .rlim_cur = x,
                                                .rlim_max = x,
                                        };

                                r = rlimit_format(&nl, &f);
                                if (r < 0)
                                        return r;

                                if (c->rlimit[ri])
                                        *c->rlimit[ri] = nl;
                                else {
                                        c->rlimit[ri] = newdup(struct rlimit, &nl, 1);
                                        if (!c->rlimit[ri])
                                                return -ENOMEM;
                                }

                                unit_write_settingf(u, flags, name, "%s=%s", name, f);
                        }

                        return 1;
                }

        } else if (streq(name, "MountImages")) {
                _cleanup_free_ char *format_str = NULL;
                MountImage *mount_images = NULL;
                size_t n_mount_images = 0;
                char *source, *destination;
                int permissive;

                r = sd_bus_message_enter_container(message, 'a', "(ssba(ss))");
                if (r < 0)
                        return r;

                for (;;) {
                        _cleanup_(mount_options_free_allp) MountOptions *options = NULL;
                        _cleanup_free_ char *source_escaped = NULL, *destination_escaped = NULL;
                        char *tuple;

                        r = sd_bus_message_enter_container(message, 'r', "ssba(ss)");
                        if (r < 0)
                                return r;

                        r = sd_bus_message_read(message, "ssb", &source, &destination, &permissive);
                        if (r <= 0)
                                break;

                        if (!path_is_absolute(source))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Source path %s is not absolute.", source);
                        if (!path_is_normalized(source))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Source path %s is not normalized.", source);
                        if (!path_is_absolute(destination))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Destination path %s is not absolute.", destination);
                        if (!path_is_normalized(destination))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Destination path %s is not normalized.", destination);

                        /* Need to store them in the unit with the escapes, so that they can be parsed again */
                        source_escaped = shell_escape(source, ":");
                        if (!source_escaped)
                                return -ENOMEM;
                        destination_escaped = shell_escape(destination, ":");
                        if (!destination_escaped)
                                return -ENOMEM;

                        tuple = strjoin(format_str,
                                        format_str ? " " : "",
                                        permissive ? "-" : "",
                                        source_escaped,
                                        ":",
                                        destination_escaped);
                        if (!tuple)
                                return -ENOMEM;
                        free_and_replace(format_str, tuple);

                        r = bus_read_mount_options(message, error, &options, &format_str, ":");
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(message);
                        if (r < 0)
                                return r;

                        r = mount_image_add(&mount_images, &n_mount_images,
                                            &(MountImage) {
                                                    .source = source,
                                                    .destination = destination,
                                                    .mount_options = options,
                                                    .ignore_enoent = permissive,
                                                    .type = MOUNT_IMAGE_DISCRETE,
                                            });
                        if (r < 0)
                                return r;
                }
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (n_mount_images == 0) {
                                c->mount_images = mount_image_free_many(c->mount_images, &c->n_mount_images);

                                unit_write_settingf(u, flags, name, "%s=", name);
                        } else {
                                for (size_t i = 0; i < n_mount_images; ++i) {
                                        r = mount_image_add(&c->mount_images, &c->n_mount_images, &mount_images[i]);
                                        if (r < 0)
                                                return r;
                                }

                                unit_write_settingf(u, flags|UNIT_ESCAPE_C|UNIT_ESCAPE_SPECIFIERS,
                                                    name,
                                                    "%s=%s",
                                                    name,
                                                    format_str);
                        }
                }

                mount_images = mount_image_free_many(mount_images, &n_mount_images);

                return 1;
        } else if (streq(name, "ExtensionImages")) {
                _cleanup_free_ char *format_str = NULL;
                MountImage *extension_images = NULL;
                size_t n_extension_images = 0;

                r = sd_bus_message_enter_container(message, 'a', "(sba(ss))");
                if (r < 0)
                        return r;

                for (;;) {
                        _cleanup_(mount_options_free_allp) MountOptions *options = NULL;
                        _cleanup_free_ char *source_escaped = NULL;
                        char *source, *tuple;
                        int permissive;

                        r = sd_bus_message_enter_container(message, 'r', "sba(ss)");
                        if (r < 0)
                                return r;

                        r = sd_bus_message_read(message, "sb", &source, &permissive);
                        if (r <= 0)
                                break;

                        if (!path_is_absolute(source))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Source path %s is not absolute.", source);
                        if (!path_is_normalized(source))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Source path %s is not normalized.", source);

                        /* Need to store them in the unit with the escapes, so that they can be parsed again */
                        source_escaped = shell_escape(source, ":");
                        if (!source_escaped)
                                return -ENOMEM;

                        tuple = strjoin(format_str,
                                        format_str ? " " : "",
                                        permissive ? "-" : "",
                                        source_escaped);
                        if (!tuple)
                                return -ENOMEM;
                        free_and_replace(format_str, tuple);

                        r = bus_read_mount_options(message, error, &options, &format_str, ":");
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(message);
                        if (r < 0)
                                return r;

                        r = mount_image_add(&extension_images, &n_extension_images,
                                            &(MountImage) {
                                                    .source = source,
                                                    .mount_options = options,
                                                    .ignore_enoent = permissive,
                                                    .type = MOUNT_IMAGE_EXTENSION,
                                            });
                        if (r < 0)
                                return r;
                }
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                        if (n_extension_images == 0) {
                                c->extension_images = mount_image_free_many(c->extension_images, &c->n_extension_images);

                                unit_write_settingf(u, flags, name, "%s=", name);
                        } else {
                                for (size_t i = 0; i < n_extension_images; ++i) {
                                        r = mount_image_add(&c->extension_images, &c->n_extension_images, &extension_images[i]);
                                        if (r < 0)
                                                return r;
                                }

                                unit_write_settingf(u, flags|UNIT_ESCAPE_C|UNIT_ESCAPE_SPECIFIERS,
                                                    name,
                                                    "%s=%s",
                                                    name,
                                                    format_str);
                        }
                }

                extension_images = mount_image_free_many(extension_images, &n_extension_images);

                return 1;

        } else if (STR_IN_SET(name, "StateDirectorySymlink", "RuntimeDirectorySymlink", "CacheDirectorySymlink", "LogsDirectorySymlink")) {
                char *source, *destination;
                ExecDirectory *directory;
                uint64_t symlink_flags; /* No flags for now, reserved for future uses. */
                ExecDirectoryType i;

                assert_se((i = exec_directory_type_symlink_from_string(name)) >= 0);
                directory = c->directories + i;

                r = sd_bus_message_enter_container(message, 'a', "(sst)");
                if (r < 0)
                        return r;

                while ((r = sd_bus_message_read(message, "(sst)", &source, &destination, &symlink_flags)) > 0) {
                        if (!path_is_valid(source))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Source path %s is not valid.", source);
                        if (path_is_absolute(source))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Source path %s is absolute.", source);
                        if (!path_is_normalized(source))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Source path %s is not normalized.", source);
                        if (!path_is_valid(destination))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Destination path %s is not valid.", destination);
                        if (path_is_absolute(destination))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Destination path %s is absolute.", destination);
                        if (!path_is_normalized(destination))
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Destination path %s is not normalized.", destination);
                        if (symlink_flags != 0)
                                return sd_bus_error_setf(error, SD_BUS_ERROR_INVALID_ARGS, "Flags must be zero.");

                        if (!UNIT_WRITE_FLAGS_NOOP(flags)) {
                                _cleanup_free_ char *destination_escaped = NULL, *source_escaped = NULL;
                                ExecDirectoryItem *item = NULL;

                                /* Adding new directories is supported from both *DirectorySymlink methods and the
                                 * older ones, so try to find an existing configuration first and create it if it's
                                 * not there yet. */
                                for (size_t j = 0; j < directory->n_items; ++j)
                                        if (path_equal(source, directory->items[j].path)) {
                                                item = &directory->items[j];
                                                break;
                                        }

                                if (item)
                                        r = strv_extend(&item->symlinks, destination);
                                else
                                        r = exec_directory_add(&directory->items, &directory->n_items, source, STRV_MAKE(destination));
                                if (r < 0)
                                        return r;

                                /* Need to store them in the unit with the escapes, so that they can be parsed again */
                                source_escaped = xescape(source, ":");
                                destination_escaped = xescape(destination, ":");
                                if (!source_escaped || !destination_escaped)
                                        return -ENOMEM;

                                unit_write_settingf(
                                                u, flags|UNIT_ESCAPE_SPECIFIERS, exec_directory_type_to_string(i),
                                                "%s=%s:%s",
                                                exec_directory_type_to_string(i),
                                                source_escaped,
                                                destination_escaped);
                        }
                }
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(message);
                if (r < 0)
                        return r;

                return 1;

        }

        return 0;
}
