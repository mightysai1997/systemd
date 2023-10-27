/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "udev-ctrl.h"
#include "varlink.h"

typedef struct UdevConnection {
        Varlink *link;
        UdevCtrl *uctrl;
        usec_t timeout;
} UdevConnection;

int udev_connection_init(UdevConnection *conn, usec_t timeout);
void udev_connection_done(UdevConnection *conn);

static inline int udev_connection_send_ping(UdevConnection *conn) {
        assert(conn);
        assert(conn->link || conn->uctrl);

        if (conn->uctrl)
                return udev_ctrl_send_ping(conn->uctrl);

        return 0;
}

static inline int udev_connection_wait(UdevConnection *conn) {
        assert(conn);
        assert(conn->link || conn->uctrl);

        if (conn->uctrl)
                return udev_ctrl_wait(conn->uctrl, conn->timeout);

        return 0;
}
