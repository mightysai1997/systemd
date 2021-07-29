/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "unit.h"

typedef struct Device Device;
typedef struct IOCostQos IOCostQos;
typedef struct IOCostModel IOCostModel;


/* A mask specifying where we have seen the device currently. This is a bitmask because the device might show up
 * asynchronously from each other at various places. For example, in very common case a device might already be mounted
 * before udev finished probing it (think: a script setting up a loopback block device, formatting it and mounting it
 * in quick succession). Hence we need to track precisely where it is already visible and where not. */
typedef enum DeviceFound {
        DEVICE_NOT_FOUND   = 0,
        DEVICE_FOUND_UDEV  = 1 << 0, /* The device has shown up in the udev database */
        DEVICE_FOUND_MOUNT = 1 << 1, /* The device has shown up in /proc/self/mountinfo */
        DEVICE_FOUND_SWAP  = 1 << 2, /* The device has shown up in /proc/swaps */
        DEVICE_FOUND_MASK  = DEVICE_FOUND_UDEV|DEVICE_FOUND_MOUNT|DEVICE_FOUND_SWAP,
} DeviceFound;

struct IOCostQos {
        IOCostCtrl ctrl;
        int enabled;
        uint32_t read_latency_percentile;
        uint32_t read_latency_threshold;
        uint32_t write_latency_percentile;
        uint32_t write_latency_threshold;
        uint32_t min;
        uint32_t max;
};

struct IOCostModel {
        IOCostCtrl ctrl;
        uint64_t rbps;
        uint64_t rseqiops;
        uint64_t rrandiops;
        uint64_t wbps;
        uint64_t wseqiops;
        uint64_t wrandiops;
};

struct Device {
        Unit meta;

        char *sysfs;
        char *devname;

        /* In order to be able to distinguish dependencies on different device nodes we might end up creating multiple
         * devices for the same sysfs path. We chain them up here. */
        LIST_FIELDS(struct Device, same_sysfs);

        DeviceState state, deserialized_state;
        DeviceFound found, deserialized_found, enumerated_found;

        bool bind_mounts;

        /* The SYSTEMD_WANTS udev property for this device the last time we saw it */
        char **wants_property;

        IOCostQos io_cost_qos;
        IOCostModel io_cost_model;
};

extern const UnitVTable device_vtable;

void device_found_node(Manager *m, const char *node, DeviceFound found, DeviceFound mask);
bool device_shall_be_bound_by(Unit *device, Unit *u);

DEFINE_CAST(DEVICE, Device);
