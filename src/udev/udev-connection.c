/* SPDX-License-Identifier: GPL-2.0-or-later */

#include "udev-connection.h"
#include "udev-varlink.h"

int udev_connection_init(UdevConnection *conn) {
        int r;

        r = udev_varlink_connect(&conn->link);
        if (r < 0)
                return log_error_errno(r, "Failed to initialize varlink connection: %m");

        r = udev_ctrl_new(&conn->uctrl);
        if (r < 0)
                return log_error_errno(r, "Failed to initialize udev control: %m");

        return 0;
}

void udev_connection_done(UdevConnection *conn) {
        if (!conn)
                return;

        conn->link = varlink_flush_close_unref(conn->link);
        conn->uctrl = udev_ctrl_unref(conn->uctrl);
}
