/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#if HAVE_LIBBPF

#include <bpf/bpf.h>
#include <bpf/btf.h>
#include <bpf/libbpf.h>

#include "bpf-compat.h"
#include "dlfcn-util.h"

DLSYM_PROTOTYPE(bpf_link__destroy);
DLSYM_PROTOTYPE(bpf_link__fd);
DLSYM_PROTOTYPE(bpf_link__open);
DLSYM_PROTOTYPE(bpf_link__pin);
DLSYM_PROTOTYPE(bpf_map__fd);
DLSYM_PROTOTYPE(bpf_map__name);
DLSYM_PROTOTYPE(bpf_map__set_inner_map_fd);
DLSYM_PROTOTYPE(bpf_map__set_max_entries);
DLSYM_PROTOTYPE(bpf_map__set_pin_path);
DLSYM_PROTOTYPE(bpf_map_delete_elem);
DLSYM_PROTOTYPE(bpf_map_get_fd_by_id);
DLSYM_PROTOTYPE(bpf_map_get_next_id);
DLSYM_PROTOTYPE(bpf_map_lookup_elem);
DLSYM_PROTOTYPE(bpf_map_update_elem);
/* The *_skeleton APIs are autogenerated by bpftool, the targets can be found
 * in ./build/src/core/bpf/socket_bind/socket-bind.skel.h */
DLSYM_PROTOTYPE(bpf_obj_get_info_by_fd);
DLSYM_PROTOTYPE(bpf_object__attach_skeleton);
DLSYM_PROTOTYPE(bpf_object__destroy_skeleton);
DLSYM_PROTOTYPE(bpf_object__detach_skeleton);
DLSYM_PROTOTYPE(bpf_object__load_skeleton);
DLSYM_PROTOTYPE(bpf_object__name);
DLSYM_PROTOTYPE(bpf_object__open_skeleton);
DLSYM_PROTOTYPE(bpf_object__pin_maps);
DLSYM_PROTOTYPE(bpf_prog_get_fd_by_id);
DLSYM_PROTOTYPE(bpf_prog_get_next_id);
DLSYM_PROTOTYPE(bpf_program__attach);
DLSYM_PROTOTYPE(bpf_program__attach_cgroup);
DLSYM_PROTOTYPE(bpf_program__attach_lsm);
DLSYM_PROTOTYPE(bpf_program__name);
DLSYM_PROTOTYPE(btf__free);
DLSYM_PROTOTYPE(btf__load_from_kernel_by_id);
DLSYM_PROTOTYPE(btf__name_by_offset);
DLSYM_PROTOTYPE(btf__type_by_id);
DLSYM_PROTOTYPE(libbpf_bpf_map_type_str);
DLSYM_PROTOTYPE(libbpf_bpf_prog_type_str);
DLSYM_PROTOTYPE(libbpf_get_error);
DLSYM_PROTOTYPE(libbpf_set_print);
DLSYM_PROTOTYPE(ring_buffer__epoll_fd);
DLSYM_PROTOTYPE(ring_buffer__free);
DLSYM_PROTOTYPE(ring_buffer__new);
DLSYM_PROTOTYPE(ring_buffer__poll);

#endif

int dlopen_bpf(void);
