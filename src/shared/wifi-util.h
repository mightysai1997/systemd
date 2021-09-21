/* SPDX-License-Identifier: LGPL-2.1-or-later */

#pragma once

#include <linux/nl80211.h>

#include "sd-netlink.h"

#include "ether-addr-util.h"

int wifi_get_interface(sd_netlink *genl, int ifindex, enum nl80211_iftype *ret_iftype, char **ret_ssid);
int wifi_get_station(sd_netlink *genl, int ifindex, struct ether_addr *ret_bssid);
