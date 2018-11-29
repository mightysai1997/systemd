#  SPDX-License-Identifier: LGPL-2.1+
#
#  This file is part of systemd.
#
#  systemd is free software; you can redistribute it and/or modify it
#  under the terms of the GNU Lesser General Public License as published by
#  the Free Software Foundation; either version 2.1 of the License, or
#  (at your option) any later version.

ACTION=="remove|unbind", GOTO="seat_late_end"

ENV{ID_SEAT}=="", ENV{ID_AUTOSEAT}=="1", ENV{ID_FOR_SEAT}!="", ENV{ID_SEAT}="seat-$env{ID_FOR_SEAT}"
ENV{ID_SEAT}=="", IMPORT{parent}="ID_SEAT"

ENV{ID_SEAT}!="", TAG+="$env{ID_SEAT}"
m4_ifdef(`HAVE_ACL',``
TAG=="uaccess", ENV{MAJOR}!="", RUN{builtin}+="uaccess"''
)m4_dnl

LABEL="seat_late_end"
