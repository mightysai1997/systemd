/* SPDX-License-Identifier: LGPL-2.1+ */
#pragma once

#include <sys/types.h>

int namespace_open(pid_t pid, int *pidns_fd, int *mntns_fd, int *netns_fd, int *userns_fd, int *root_fd);
int namespace_enter(int pidns_fd, int mntns_fd, int netns_fd, int userns_fd, int root_fd);

int fd_is_network_ns(int fd);

int detach_mount_namespace(void);

int bind_mount_in_namespace(pid_t target, const char *propagate_path, const char *incoming_path, const char *src, const char *dest, int read_only, int make_file_or_directory, char **inaccessible_paths, char **error_path);
