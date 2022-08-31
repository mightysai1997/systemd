/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#if HAVE_LIBBPF

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

extern struct bpf_link* (*sym_bpf_program__attach_cgroup)(struct bpf_program *, int);
extern struct bpf_link* (*sym_bpf_program__attach_lsm)(struct bpf_program *);
extern int (*sym_bpf_link__fd)(const struct bpf_link *);
extern int (*sym_bpf_link__destroy)(struct bpf_link *);
extern int (*sym_bpf_map__fd)(const struct bpf_map *);
extern const char* (*sym_bpf_map__name)(const struct bpf_map *);
extern int (*sym_bpf_map_create)(enum bpf_map_type,  const char *, __u32, __u32, __u32, const struct bpf_map_create_opts *);
extern int (*sym_bpf_map__set_max_entries)(struct bpf_map *, __u32);
extern int (*sym_bpf_map_update_elem)(int, const void *, const void *, __u64);
extern int (*sym_bpf_map_delete_elem)(int, const void *);
extern int (*sym_bpf_map__set_inner_map_fd)(struct bpf_map *, int);
/* The *_skeleton APIs are autogenerated by bpftool, the targets can be found
 * in ./build/src/core/bpf/socket_bind/socket-bind.skel.h */
extern int (*sym_bpf_object__open_skeleton)(struct bpf_object_skeleton *, const struct bpf_object_open_opts *);
extern int (*sym_bpf_object__load_skeleton)(struct bpf_object_skeleton *);
extern int (*sym_bpf_object__attach_skeleton)(struct bpf_object_skeleton *);
extern void (*sym_bpf_object__detach_skeleton)(struct bpf_object_skeleton *);
extern void (*sym_bpf_object__destroy_skeleton)(struct bpf_object_skeleton *);
extern const char* (*sym_bpf_program__name)(const struct bpf_program *);
extern bool (*sym_libbpf_probe_prog_type)(enum bpf_prog_type, const void *);
extern libbpf_print_fn_t (*sym_libbpf_set_print)(libbpf_print_fn_t);
extern long (*sym_libbpf_get_error)(const void *);

#endif

int dlopen_bpf(void);
