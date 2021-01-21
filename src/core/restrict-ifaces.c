/* SPDX-License-Identifier: LGPL-2.1+ */

#include <net/if.h>

#if HAVE_LIBBPF
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#endif

#include "restrict-ifaces.h"

#include "fd-util.h"
#include "unit.h"

#if BPF_FRAMEWORK
/* libbpf, clang and llc compile time dependencies are satisfied */

#include "bpf/restrict_ifaces/restrict-ifaces-skel.h"

static struct restrict_ifaces_bpf *restrict_ifaces_bpf_free(struct restrict_ifaces_bpf *obj) {
        if (obj && libbpf_get_error(obj) == 0)
                (void) restrict_ifaces_bpf__destroy(obj);

        return NULL;
}

DEFINE_TRIVIAL_CLEANUP_FUNC(struct restrict_ifaces_bpf *, restrict_ifaces_bpf_free);

static int prepare_restrict_ifaces_bpf(Unit* u, bool is_allow_list,
                const Set *restrict_network_interfaces,
                struct restrict_ifaces_bpf **ret_object) {
        _cleanup_(restrict_ifaces_bpf_freep) struct restrict_ifaces_bpf *obj = NULL;
        char *iface;
        uint8_t dummy = 0;
        uint32_t ifindex;
        int r, map_fd;

        assert(ret_object);

        obj = restrict_ifaces_bpf__open();
        if (!obj)
                return log_unit_error_errno(u, r, "Failed to open BPF object");

        r = bpf_map__resize(obj->maps.ifaces_map, MAX(set_size(restrict_network_interfaces), 1u));
        if (r != 0)
                return log_unit_error_errno(u, r,
                                "Failed to resize BPF map '%s': %m",
                                bpf_map__name(obj->maps.ifaces_map));

        obj->rodata->is_allow_list = (__u8) is_allow_list;

        r = restrict_ifaces_bpf__load(obj);
        if (r != 0)
                return log_unit_error_errno(u, r, "Failed to load BPF object");

        map_fd = bpf_map__fd(obj->maps.ifaces_map);
        if (map_fd < 0)
                return log_unit_error_errno(u, map_fd, "Failed to get BPF map fd");


        /* Key zero indicates whether this is an allow or deny-list approach */
        ifindex = 0;
        dummy = (uint8_t) is_allow_list;
        r = bpf_map_update_elem(map_fd, &ifindex, &dummy, BPF_ANY);
        if (r < 0)
                return log_unit_error_errno(u, r, "Failed to update BPF map '%s' fd: %m", map_name);
        dummy = 0;

        SET_FOREACH(iface, restrict_network_interfaces) {
                ifindex = if_nametoindex(iface);
                if (ifindex == 0) {
                        log_unit_warning(u, "Could't find index of network interface, ignoring '%s'", iface);
                        continue;
                }

                r = bpf_map_update_elem(map_fd, &ifindex, &dummy, BPF_ANY);
                if (r != 0)
                        return log_unit_error_errno(u, r, "Failed to update BPF map '%s' fd: %m", bpf_map__name(obj->maps.ifaces_map));
        }

        *ret_object = TAKE_PTR(obj);
        return 0;
}

static int can_link_bpf_program(struct bpf_program *prog) {
        _cleanup_(bpf_link_freep) struct bpf_link *link = NULL;
        int err;

        assert(prog);

        link = bpf_program__attach_cgroup(prog, -1);
        if (!link)
                return -ENOMEM;

        err = libbpf_get_error(link);
        if (err != -EINVAL && err != -EBADF)
                return err;

        /* EBADF indicates that bpf_link is supported by kernel. */
        return err == -EBADF;
}

int restrict_network_interfaces_supported(void) {
        _cleanup_(restrict_ifaces_bpf_freep) struct restrict_ifaces_bpf *obj = NULL;
        int r;
        static int supported = -1;

        if (supported != -1)
                return supported;

        r = cg_unified_controller(SYSTEMD_CGROUP_CONTROLLER);
        if (r < 0) {
                log_error_errno(r, "Can't determine whether the unified hierarchy is used: %m");
                supported = 0;
                return supported;
        }
        if (r == 0) {
                log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                "Not running with unified cgroup hierarchy, BPF is not supported");
                supported = 0;
                return supported;
        }

        if (!bpf_probe_prog_type(BPF_PROG_TYPE_CGROUP_SKB, /*ifindex=*/0)) {
                log_debug_errno(SYNTHETIC_ERRNO(EOPNOTSUPP),
                                "BPF program type cgroup_skb is not supported");
                supported = 0;
                return supported;
        }

        r = prepare_restrict_ifaces_bpf(NULL, true, NULL, &obj);
        if (r < 0)
                return log_debug_errno(r, "Failed to load BPF object: %m");

        supported = can_link_bpf_program(obj->progs.restrict_network_interfaces_ingress);
        return supported;
}

static int restrict_network_interfaces_install_impl(Unit *u) {
        _cleanup_(bpf_link_freep) struct bpf_link *egress_link = NULL, *ingress_link = NULL;
        _cleanup_(restrict_ifaces_bpf_freep) struct restrict_ifaces_bpf *obj = NULL;
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

        if (!cc->restrict_network_interfaces)
                return 0;

        r = prepare_restrict_ifaces_bpf(u,
                cc->restrict_network_interfaces_is_allow_list,
                cc->restrict_network_interfaces,
                &obj);
        if (r < 0)
                return r;

        cgroup_fd = open(cgroup_path, O_RDONLY | O_CLOEXEC, 0);
        if (cgroup_fd < 0)
                return -errno;

        ingress_link = bpf_program__attach_cgroup(obj->progs.restrict_network_interfaces_ingress, cgroup_fd);
        if (libbpf_get_error(ingress_link))
                return log_unit_error_errno(u, r, "Failed to create ingress cgroup link");

        egress_link = bpf_program__attach_cgroup(obj->progs.restrict_network_interfaces_egress, cgroup_fd);
        if (libbpf_get_error(egress_link))
                return log_unit_error_errno(u, r, "Failed to create egress cgroup link");

        u->restrict_ifaces_ingress_bpf_link = TAKE_PTR(ingress_link);
        u->restrict_ifaces_egress_bpf_link = TAKE_PTR(egress_link);

        return 0;
}

int restrict_network_interfaces_install(Unit *u) {
        int r = restrict_network_interfaces_install_impl(u);
        fdset_close(u->restrict_ifaces_restored_fds);
        return r;
}
#else /* ! BPF_FRAMEWORK */
int restrict_network_interfaces_supported(void) {
        return 0;
}

int restrict_network_interfaces_install(Unit *u) {
        return log_unit_debug_errno(u, SYNTHETIC_ERRNO(EOPNOTSUPP),
                        "Failed to install IfacesLock: BPF programs built from source code are not supported: %m");
}
#endif
