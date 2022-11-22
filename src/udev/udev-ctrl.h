/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "sd-event.h"

#include "macro.h"
#include "time-util.h"
#include "varlink.h"

typedef struct UdevCtrl UdevCtrl;

typedef enum UdevCtrlMessageType {
        _UDEV_CTRL_END_MESSAGES,
} UdevCtrlMessageType;

typedef union UdevCtrlMessageValue {
        int intval;
        char buf[256];
} UdevCtrlMessageValue;

typedef int (*udev_ctrl_handler_t)(UdevCtrl *udev_ctrl, UdevCtrlMessageType type,
                                   const UdevCtrlMessageValue *value, void *userdata);

int udev_ctrl_new_from_fd(UdevCtrl **ret, int fd);
int udev_ctrl_new_with_link(UdevCtrl **ret, Varlink *link);

int udev_ctrl_enable_receiving(UdevCtrl *uctrl);
UdevCtrl *udev_ctrl_ref(UdevCtrl *uctrl);
UdevCtrl *udev_ctrl_unref(UdevCtrl *uctrl);
int udev_ctrl_attach_event(UdevCtrl *uctrl, sd_event *event);
int udev_ctrl_start(UdevCtrl *uctrl, udev_ctrl_handler_t callback, void *userdata);
sd_event_source *udev_ctrl_get_event_source(UdevCtrl *uctrl);

int udev_ctrl_send(UdevCtrl *uctrl, UdevCtrlMessageType type, const void *data);

DEFINE_TRIVIAL_CLEANUP_FUNC(UdevCtrl*, udev_ctrl_unref);
