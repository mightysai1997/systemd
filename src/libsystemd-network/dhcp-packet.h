/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dhcp-protocol.h"

int dhcp_message_init(
                DHCPMessage *message,
                uint8_t op,
                uint32_t xid,
                uint8_t type,
                uint16_t arp_type,
                uint8_t hlen,
                const uint8_t *chaddr,
                size_t optlen,
                size_t *optoffset);

uint16_t dhcp_packet_checksum(uint8_t *buf, size_t len);

void dhcp_packet_append_ip_headers(
                DHCPPacket *packet,
                be32_t source_addr,
                uint16_t source,
                be32_t destination_addr,
                uint16_t destination,
                uint16_t len,
                int ip_service_type);

int dhcp_packet_verify_headers(DHCPPacket *packet, size_t len, bool checksum, uint16_t port);
