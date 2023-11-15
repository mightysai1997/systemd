/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "alloc-util.h"
#include "dhcp-client-id-internal.h"
#include "unaligned.h"
#include "utf8.h"

int sd_dhcp_client_id_new(sd_dhcp_client_id **ret) {
        sd_dhcp_client_id *client_id;

        assert_return(ret, -EINVAL);

        client_id = new0(sd_dhcp_client_id, 1);
        if (!client_id)
                return -ENOMEM;

        *ret = client_id;
        return 0;
}

sd_dhcp_client_id* sd_dhcp_client_id_free(sd_dhcp_client_id *client_id) {
        return mfree(client_id);
}

int sd_dhcp_client_id_clear(sd_dhcp_client_id *client_id) {
        assert_return(client_id, -EINVAL);

        *client_id = (sd_dhcp_client_id) {};
        return 0;
}

int sd_dhcp_client_id_is_set(sd_dhcp_client_id *client_id) {
        if (!client_id)
                return false;

        return client_id->size >= MIN_CLIENT_ID_LEN && client_id->size <= MAX_CLIENT_ID_LEN;
}

int sd_dhcp_client_id_get(sd_dhcp_client_id *client_id, uint8_t *ret_type, const void **ret_data, size_t *ret_size) {
        assert_return(sd_dhcp_client_id_is_set(client_id), -EINVAL);
        assert_return(ret_type, -EINVAL);
        assert_return(ret_data, -EINVAL);
        assert_return(ret_size, -EINVAL);

        *ret_type = client_id->id.type;
        *ret_data = client_id->id.raw.data;
        *ret_size = client_id->size - 1;
        return 0;
}

int sd_dhcp_client_id_get_raw(sd_dhcp_client_id *client_id, const void **ret_data, size_t *ret_size) {
        assert_return(sd_dhcp_client_id_is_set(client_id), -EINVAL);
        assert_return(ret_data, -EINVAL);
        assert_return(ret_size, -EINVAL);

        /* Unlike sd_dhcp_client_id_get(), this returns whole client ID including its type. */

        *ret_data = &client_id->id;
        *ret_size = client_id->size - 1;
        return 0;
}

int sd_dhcp_client_id_set(
                sd_dhcp_client_id *client_id,
                uint8_t type,
                const void *data,
                size_t data_size) {

        assert_return(client_id, -EINVAL);
        assert_return(data, -EINVAL);
        assert_return(data_size >= MIN_CLIENT_ID_DATA_LEN && data_size <= MAX_CLIENT_ID_DATA_LEN, -EINVAL);

        client_id->id.type = type;
        memcpy(client_id->id.raw.data, data, data_size);

        client_id->size = offsetof(typeof(client_id->id), raw.data) + data_size;
        return 0;
}

int sd_dhcp_client_id_set_raw(
                sd_dhcp_client_id *client_id,
                const void *data,
                size_t data_size) {

        assert_return(client_id, -EINVAL);
        assert_return(data, -EINVAL);
        assert_return(data_size >= MIN_CLIENT_ID_LEN && data_size <= MAX_CLIENT_ID_LEN, -EINVAL);

        /* Unlike sd_dhcp_client_id_set(), this takes whole client ID including its type. */

        memcpy(&client_id->id, data, data_size);

        client_id->size = data_size;
        return 0;
}

int sd_dhcp_client_id_set_iaid_duid(
                sd_dhcp_client_id *client_id,
                uint32_t iaid,
                sd_dhcp_duid *duid) {

        assert_return(client_id, -EINVAL);
        assert_return(duid, -EINVAL);
        assert_return(sd_dhcp_duid_is_set(duid), -ESTALE);

        client_id->id.type = 255;
        unaligned_write_be32(&client_id->id.ns.iaid, iaid);
        memcpy(&client_id->id.ns.duid, &duid->duid, duid->size);

        client_id->size = offsetof(typeof(client_id->id), ns.duid) + duid->size;
        return 0;
}

int sd_dhcp_client_id_to_string(sd_dhcp_client_id *client_id, char **ret) {
        _cleanup_free_ char *t = NULL;
        size_t len;
        int r;

        assert_return(sd_dhcp_client_id_is_set(client_id), -EINVAL);
        assert_return(ret, -EINVAL);

        len = client_id->size - offsetof(typeof(client_id->id), type);

        switch (client_id->id.type) {
        case 0:
                if (utf8_is_printable((char *) client_id->id.gen.data, len))
                        r = asprintf(&t, "%.*s", (int) len, client_id->id.gen.data);
                else
                        r = asprintf(&t, "DATA");
                break;
        case 1:
                if (len == sizeof_field(sd_dhcp_client_id, id.eth))
                        r = asprintf(&t, "%02x:%02x:%02x:%02x:%02x:%02x",
                                     client_id->id.eth.haddr[0],
                                     client_id->id.eth.haddr[1],
                                     client_id->id.eth.haddr[2],
                                     client_id->id.eth.haddr[3],
                                     client_id->id.eth.haddr[4],
                                     client_id->id.eth.haddr[5]);
                else
                        r = asprintf(&t, "ETHER");
                break;
        case 2 ... 254:
                r = asprintf(&t, "ARP/LL");
                break;
        case 255:
                if (len < sizeof(uint32_t))
                        r = asprintf(&t, "IAID/DUID");
                else {
                        uint32_t iaid = be32toh(client_id->id.ns.iaid);
                        /* TODO: check and stringify DUID */
                        r = asprintf(&t, "IAID:0x%x/DUID", iaid);
                }
                break;
        }
        if (r < 0)
                return -ENOMEM;

        *ret = TAKE_PTR(t);
        return 0;
}

int sd_dhcp_client_id_to_string_from_raw(const void *data, size_t data_size, char **ret) {
        sd_dhcp_client_id client_id;
        int r;

        assert_return(data, -EINVAL);
        assert_return(data_size >= MIN_CLIENT_ID_LEN && data_size <= MAX_CLIENT_ID_LEN, -EINVAL);
        assert_return(ret, -EINVAL);

        r = sd_dhcp_client_id_set_raw(&client_id, data, data_size);
        if (r < 0)
                return r;

        return sd_dhcp_client_id_to_string(&client_id, ret);
}
