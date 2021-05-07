/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include "sd-event.h"

#include "networkd-link.h"

typedef struct Request Request;

typedef int (*request_after_configure_handler_t)(Request*, void*);
typedef void (*request_on_free_handler_t)(Request*);

typedef enum RequestType {
        _REQUEST_TYPE_MAX,
        _REQUEST_TYPE_INVALID = -EINVAL,
} RequestType;

typedef struct Request {
        Link *link;
        RequestType type;
        bool consume_object;
        void *object;
        void *userdata;
        unsigned *message_counter;
        link_netlink_message_handler_t netlink_handler;
        request_after_configure_handler_t after_configure;
        request_on_free_handler_t on_free;
} Request;

Request *request_free(Request *req);
void request_drop(Request *req);

int link_queue_request(
                Link *link,
                RequestType type,
                void *object,
                bool consume_object,
                unsigned *message_counter,
                link_netlink_message_handler_t netlink_handler,
                Request **ret);

int manager_process_requests(sd_event_source *s, void *userdata);
