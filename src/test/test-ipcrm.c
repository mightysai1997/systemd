/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

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

#include "clean-ipc.h"
#include "user-util.h"
#include "util.h"

int main(int argc, char *argv[]) {
        uid_t uid;

        assert_se(argc == 2);
        assert_se(parse_uid(argv[1], &uid) >= 0);

        return clean_ipc(uid) < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
