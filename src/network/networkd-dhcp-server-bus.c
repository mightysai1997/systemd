/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "sd-dhcp-server.h"

#include "alloc-util.h"
#include "bus-common-errors.h"
#include "bus-util.h"
#include "dhcp-server-internal.h"
#include "networkd-dhcp-server-bus.h"
#include "networkd-link-bus.h"
#include "networkd-manager.h"
#include "strv.h"

static int property_get_leases(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {
        Link *l = userdata;
        sd_dhcp_server *s;
        DHCPLease *lease;
        int r;

        assert(reply);
        assert(l);

        s = l->dhcp_server;
        if (!s)
                return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Link %s has no DHCP server.", l->ifname);

        if (!in4_addr_is_set(&s->relay_target))
                return sd_bus_error_setf(error, SD_BUS_ERROR_NOT_SUPPORTED, "Link %s has DHCP relay agent active.", l->ifname);


        r = sd_bus_message_open_container(reply, 'a', "(uayayayayt)");
        if (r < 0)
                return r;

        HASHMAP_FOREACH(lease, s->leases_by_client_id) {
                r = sd_bus_message_open_container(reply, 'r', "uayayayayt");
                if (r < 0)
                        return r;

                r = sd_bus_message_append(reply, "u", (uint32_t)AF_INET);
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', lease->client_id.data, lease->client_id.length);
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', &lease->address, sizeof(lease->address));
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', &lease->gateway, sizeof(lease->gateway));
                if (r < 0)
                        return r;

                r = sd_bus_message_append_array(reply, 'y', &lease->chaddr, sizeof(lease->chaddr));
                if (r < 0)
                        return r;

                r = sd_bus_message_append_basic(reply, 't', &lease->expiration);
                if (r < 0)
                        return r;

                r = sd_bus_message_close_container(reply);
                if (r < 0)
                        return r;
        }

        return sd_bus_message_close_container(reply);
}

static int dhcp_server_emit_changed(Link *link, const char *property, ...) {
        _cleanup_free_ char *path = NULL;
        char **l;

        assert(link);

        path = link_bus_path(link);
        if (!path)
                return log_oom();

        l = strv_from_stdarg_alloca(property);

        return sd_bus_emit_properties_changed_strv(
                        link->manager->bus,
                        path,
                        "org.freedesktop.network1.DHCPServer",
                        l);
}

void dhcp_server_callback(sd_dhcp_server *s, uint64_t event, void *data) {
        Link *l = data;

        assert(l);

        if (event & SD_DHCP_SERVER_EVENT_LEASE_CHANGED)
                (void) dhcp_server_emit_changed(l, "Leases", NULL);
}


const sd_bus_vtable dhcp_server_vtable[] = {
        SD_BUS_VTABLE_START(0),

        SD_BUS_PROPERTY("Leases", "a(uayayayayt)", property_get_leases, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),

        SD_BUS_VTABLE_END
};
