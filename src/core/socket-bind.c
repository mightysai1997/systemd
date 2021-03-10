/* SPDX-License-Identifier: LGPL-2.1+ */

#if HAVE_LIBBPF
#include <bpf/bpf.h>
#include "bpf-link.h"
#endif

#include "socket-bind.h"
#include "fd-util.h"

#if BPF_FRAMEWORK
/* libbpf, clang, llvm and bpftool compile time dependencies are satisfied */
#include "bpf/socket_bind/socket-bind.skel.h"
#include "bpf/socket_bind/socket-bind-api.bpf.h"

static struct socket_bind_bpf *socket_bind_bpf_free(struct socket_bind_bpf *obj) {
        /* socket_bind_bpf__destroy handles object == NULL case */
        (void) socket_bind_bpf__destroy(obj);

        return NULL;
}

DEFINE_TRIVIAL_CLEANUP_FUNC(struct socket_bind_bpf *, socket_bind_bpf_free);

static int update_rules_map(
                int map_fd, CGroupSocketBindItem *head) {
        CGroupSocketBindItem *item;
        uint32_t i = 0;

        assert(map_fd >= 0);

        LIST_FOREACH(socket_bind_items, item, head) {
                const uint32_t key = i++;
                struct socket_bind_rule val = {
                        .address_family = (uint32_t) item->address_family,
                        .nr_ports = item->nr_ports,
                        .port_min = item->port_min,
                };

                if (bpf_map_update_elem(map_fd, &key, &val, BPF_ANY) != 0)
                        return -errno;
        }

        return 0;
}

static int prepare_socket_bind_bpf(
                Unit *u, CGroupSocketBindItem *allow, CGroupSocketBindItem *deny, struct socket_bind_bpf **ret_obj) {
        _cleanup_(socket_bind_bpf_freep) struct socket_bind_bpf *obj = 0;
        uint32_t allow_count = 0, deny_count = 0;
        int allow_map_fd, deny_map_fd, r;
        CGroupSocketBindItem *item;

        assert(ret_obj);

        LIST_FOREACH(socket_bind_items, item,  allow)
                allow_count++;

        LIST_FOREACH(socket_bind_items, item, deny)
                deny_count++;

        if (allow_count > SOCKET_BIND_MAX_RULES)
                return log_unit_error_errno(u, SYNTHETIC_ERRNO(EINVAL),
                                "Maximum number of socket bind rules=%u is exceeded", SOCKET_BIND_MAX_RULES);

        if (deny_count > SOCKET_BIND_MAX_RULES)
                return log_unit_error_errno(u, SYNTHETIC_ERRNO(EINVAL),
                                "Maximum number of socket bind rules=%u is exceeded", SOCKET_BIND_MAX_RULES);

        obj = socket_bind_bpf__open();
        if (!obj)
                return log_unit_error_errno(u, SYNTHETIC_ERRNO(ENOMEM), "Failed to open BPF object");

        if (bpf_map__resize(obj->maps.sd_bind_allow, MAX(allow_count, 1u)) != 0)
                return log_unit_error_errno(u, errno,
                                "Failed to resize BPF map '%s': %m", bpf_map__name(obj->maps.sd_bind_allow));

        if (bpf_map__resize(obj->maps.sd_bind_deny, MAX(deny_count, 1u)) != 0)
                return log_unit_error_errno(u, errno,
                                "Failed to resize BPF map '%s': %m", bpf_map__name(obj->maps.sd_bind_deny));

        if (socket_bind_bpf__load(obj) != 0)
                return log_unit_error_errno(u, errno, "Failed to load BPF object");

        allow_map_fd = bpf_map__fd(obj->maps.sd_bind_allow);
        assert(allow_map_fd >= 0);

        r = update_rules_map(allow_map_fd, allow);
        if (r < 0)
                return log_unit_error_errno(
                                u, r, "Failed to put socket bind allow rules into BPF map '%s'",
                                bpf_map__name(obj->maps.sd_bind_allow));

        deny_map_fd = bpf_map__fd(obj->maps.sd_bind_deny);
        assert(deny_map_fd >= 0);

        r = update_rules_map(deny_map_fd, deny);
        if (r < 0)
                return log_unit_error_errno(
                                u, r, "Failed to put socket bind deny rules into BPF map '%s'",
                                bpf_map__name(obj->maps.sd_bind_deny));

        *ret_obj = TAKE_PTR(obj);
        return 0;
}

int socket_bind_supported(void) {
        _cleanup_(socket_bind_bpf_freep) struct socket_bind_bpf *obj = NULL;

        int r = cg_unified_controller(SYSTEMD_CGROUP_CONTROLLER);
        if (r < 0)
                return log_error_errno(r, "Can't determine whether the unified hierarchy is used: %m");

        if (r == 0) {
                log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                "Not running with unified cgroup hierarchy, BPF is not supported");
                return 0;
        }

        if (!bpf_probe_prog_type(BPF_PROG_TYPE_CGROUP_SOCK_ADDR, /*ifindex=*/0)) {
                log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                "BPF program type cgroup_sock_addr is not supported");
                return 0;
        }

        r = prepare_socket_bind_bpf(/*unit=*/NULL, /*allow_rules=*/NULL, /*deny_rules=*/NULL, &obj);
        if (r < 0) {
                log_debug_errno(r, "BPF based socket_bind is not supported: %m");
                return 0;
        }

        return can_link_bpf_program(obj->progs.sd_bind4);
}

int socket_bind_install(Unit *u) {
        _cleanup_(bpf_link_freep) struct bpf_link *ipv4 = NULL, *ipv6 = NULL;
        _cleanup_(socket_bind_bpf_freep) struct socket_bind_bpf *obj = NULL;
        _cleanup_free_ char *cgroup_path = NULL;
        _cleanup_close_ int cgroup_fd = -1;
        CGroupContext *cc;
        int r;

        cc = unit_get_cgroup_context(u);
        if (!cc)
                return 0;

        r = cg_get_path(SYSTEMD_CGROUP_CONTROLLER, u->cgroup_path, NULL, &cgroup_path);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to get cgroup path: %m");

        if (!cc->socket_bind_allow && !cc->socket_bind_deny)
                return 0;

        r = prepare_socket_bind_bpf(u, cc->socket_bind_allow, cc->socket_bind_deny, &obj);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to load BPF object: %m");

        cgroup_fd = open(cgroup_path, O_RDONLY | O_CLOEXEC, 0);
        if (cgroup_fd < 0)
                return log_unit_error_errno(
                                u, errno, "Failed to open cgroup=%s for reading", cgroup_path);

        ipv4 = bpf_program__attach_cgroup(obj->progs.sd_bind4, cgroup_fd);
        r = libbpf_get_error(ipv4);
        if (r != 0)
                return log_unit_error_errno(u, r, "Failed to link '%s' cgroup-bpf program",
                                bpf_program__name(obj->progs.sd_bind4));

        ipv6 = bpf_program__attach_cgroup(obj->progs.sd_bind6, cgroup_fd);
        r = libbpf_get_error(ipv6);
        if (r != 0)
                return log_unit_error_errno(u, r, "Failed to link '%s' cgroup-bpf program",
                                bpf_program__name(obj->progs.sd_bind6));

        u->ipv4_socket_bind_link = TAKE_PTR(ipv4);
        u->ipv6_socket_bind_link = TAKE_PTR(ipv6);

        return 0;
}
#else /* ! BPF_FRAMEWORK */
int socket_bind_supported(void) {
        return 0;
}

int socket_bind_install(Unit *u) {
        log_unit_debug(u, "Failed to install socket bind: BPF framework is not supported");
        return 0;
}

#endif
