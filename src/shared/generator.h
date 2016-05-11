#pragma once

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdio.h>

int generator_write_fsck_deps(
        FILE *f,
        const char *dir,
        const char *what,
        const char *where,
        const char *type);

int generator_write_timeouts(
        const char *dir,
        const char *what,
        const char *where,
        const char *opts,
        char **filtered);

int generator_write_initrd_root_device_deps(
        const char *dir,
        const char *what);
