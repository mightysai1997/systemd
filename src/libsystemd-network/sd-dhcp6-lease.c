/* SPDX-License-Identifier: LGPL-2.1-or-later */
/***
  Copyright © 2014-2015 Intel Corporation. All rights reserved.
***/

#include <errno.h>

#include "alloc-util.h"
#include "dhcp6-lease-internal.h"
#include "dhcp6-protocol.h"
#include "strv.h"
#include "util.h"

int dhcp6_lease_ia_rebind_expire(const DHCP6IA *ia, uint32_t *expire) {
        DHCP6Address *addr;
        uint32_t valid = 0, t;

        assert_return(ia, -EINVAL);
        assert_return(expire, -EINVAL);

        LIST_FOREACH(addresses, addr, ia->addresses) {
                t = be32toh(addr->iaaddr.lifetime_valid);
                if (valid < t)
                        valid = t;
        }

        t = be32toh(ia->ia_na.lifetime_t2);
        if (t > valid)
                return -EINVAL;

        *expire = valid - t;

        return 0;
}

DHCP6IA *dhcp6_lease_free_ia(DHCP6IA *ia) {
        DHCP6Address *address;

        if (!ia)
                return NULL;

        while (ia->addresses) {
                address = ia->addresses;

                LIST_REMOVE(addresses, ia->addresses, address);

                free(address);
        }

        return NULL;
}

int dhcp6_lease_set_serverid(sd_dhcp6_lease *lease, const uint8_t *id,
                             size_t len) {
        uint8_t *serverid;

        assert_return(lease, -EINVAL);
        assert_return(id, -EINVAL);

        serverid = memdup(id, len);
        if (!serverid)
                return -ENOMEM;

        free_and_replace(lease->serverid, serverid);
        lease->serverid_len = len;

        return 0;
}

int dhcp6_lease_get_serverid(sd_dhcp6_lease *lease, uint8_t **id, size_t *len) {
        assert_return(lease, -EINVAL);

        if (!lease->serverid)
                return -ENOMSG;

        if (id)
                *id = lease->serverid;
        if (len)
                *len = lease->serverid_len;

        return 0;
}

int dhcp6_lease_set_preference(sd_dhcp6_lease *lease, uint8_t preference) {
        assert_return(lease, -EINVAL);

        lease->preference = preference;

        return 0;
}

int dhcp6_lease_get_preference(sd_dhcp6_lease *lease, uint8_t *preference) {
        assert_return(preference, -EINVAL);

        if (!lease)
                return -EINVAL;

        *preference = lease->preference;

        return 0;
}

int dhcp6_lease_set_rapid_commit(sd_dhcp6_lease *lease) {
        assert_return(lease, -EINVAL);

        lease->rapid_commit = true;

        return 0;
}

int dhcp6_lease_get_rapid_commit(sd_dhcp6_lease *lease, bool *rapid_commit) {
        assert_return(lease, -EINVAL);
        assert_return(rapid_commit, -EINVAL);

        *rapid_commit = lease->rapid_commit;

        return 0;
}

int dhcp6_lease_get_iaid(sd_dhcp6_lease *lease, be32_t *iaid) {
        assert_return(lease, -EINVAL);
        assert_return(iaid, -EINVAL);

        *iaid = lease->ia.ia_na.id;

        return 0;
}

int dhcp6_lease_get_pd_iaid(sd_dhcp6_lease *lease, be32_t *iaid) {
        assert_return(lease, -EINVAL);
        assert_return(iaid, -EINVAL);

        *iaid = lease->pd.ia_pd.id;

        return 0;
}

int sd_dhcp6_lease_get_address(sd_dhcp6_lease *lease, struct in6_addr *addr,
                               uint32_t *lifetime_preferred,
                               uint32_t *lifetime_valid) {
        assert_return(lease, -EINVAL);
        assert_return(addr, -EINVAL);
        assert_return(lifetime_preferred, -EINVAL);
        assert_return(lifetime_valid, -EINVAL);

        if (!lease->addr_iter)
                return -ENOMSG;

        memcpy(addr, &lease->addr_iter->iaaddr.address,
                sizeof(struct in6_addr));
        *lifetime_preferred =
                be32toh(lease->addr_iter->iaaddr.lifetime_preferred);
        *lifetime_valid = be32toh(lease->addr_iter->iaaddr.lifetime_valid);

        lease->addr_iter = lease->addr_iter->addresses_next;

        return 0;
}

void sd_dhcp6_lease_reset_address_iter(sd_dhcp6_lease *lease) {
        if (lease)
                lease->addr_iter = lease->ia.addresses;
}

int sd_dhcp6_lease_get_pd(sd_dhcp6_lease *lease, struct in6_addr *prefix,
                          uint8_t *prefix_len,
                          uint32_t *lifetime_preferred,
                          uint32_t *lifetime_valid) {
        assert_return(lease, -EINVAL);
        assert_return(prefix, -EINVAL);
        assert_return(prefix_len, -EINVAL);
        assert_return(lifetime_preferred, -EINVAL);
        assert_return(lifetime_valid, -EINVAL);

        if (!lease->prefix_iter)
                return -ENOMSG;

        memcpy(prefix, &lease->prefix_iter->iapdprefix.address,
               sizeof(struct in6_addr));
        *prefix_len = lease->prefix_iter->iapdprefix.prefixlen;
        *lifetime_preferred =
                be32toh(lease->prefix_iter->iapdprefix.lifetime_preferred);
        *lifetime_valid =
                be32toh(lease->prefix_iter->iapdprefix.lifetime_valid);

        lease->prefix_iter = lease->prefix_iter->addresses_next;

        return 0;
}

void sd_dhcp6_lease_reset_pd_prefix_iter(sd_dhcp6_lease *lease) {
        if (lease)
                lease->prefix_iter = lease->pd.addresses;
}

int dhcp6_lease_set_dns(sd_dhcp6_lease *lease, uint8_t *optval, size_t optlen) {
        int r;

        assert_return(lease, -EINVAL);
        assert_return(optval, -EINVAL);

        if (!optlen)
                return 0;

        r = dhcp6_option_parse_ip6addrs(optval, optlen, &lease->dns,
                                        lease->dns_count);
        if (r < 0)
                return r;

        lease->dns_count = r;

        return 0;
}

int sd_dhcp6_lease_get_dns(sd_dhcp6_lease *lease, const struct in6_addr **addrs) {
        assert_return(lease, -EINVAL);
        assert_return(addrs, -EINVAL);

        if (lease->dns_count) {
                *addrs = lease->dns;
                return lease->dns_count;
        }

        return -ENOENT;
}

int dhcp6_lease_set_domains(sd_dhcp6_lease *lease, uint8_t *optval,
                            size_t optlen) {
        int r;
        char **domains;

        assert_return(lease, -EINVAL);
        assert_return(optval, -EINVAL);

        if (!optlen)
                return 0;

        r = dhcp6_option_parse_domainname_list(optval, optlen, &domains);
        if (r < 0)
                return 0;

        strv_free_and_replace(lease->domains, domains);
        lease->domains_count = r;

        return r;
}

int sd_dhcp6_lease_get_domains(sd_dhcp6_lease *lease, char ***domains) {
        assert_return(lease, -EINVAL);
        assert_return(domains, -EINVAL);

        if (lease->domains_count) {
                *domains = lease->domains;
                return lease->domains_count;
        }

        return -ENOENT;
}

int dhcp6_lease_set_ntp(sd_dhcp6_lease *lease, uint8_t *optval, size_t optlen) {
        int r;

        assert_return(lease, -EINVAL);
        assert_return(optval, -EINVAL);

        lease->ntp = mfree(lease->ntp);
        lease->ntp_count = 0;

        for (size_t offset = 0; offset < optlen;) {
                const uint8_t *subval;
                size_t sublen;
                uint16_t subopt;

                r = dhcp6_option_parse(optval, optlen, &offset, &subopt, &sublen, &subval);
                if (r < 0)
                        return r;

                switch(subopt) {
                case DHCP6_NTP_SUBOPTION_SRV_ADDR:
                case DHCP6_NTP_SUBOPTION_MC_ADDR:
                        if (sublen != 16)
                                return 0;

                        r = dhcp6_option_parse_ip6addrs(subval, sublen, &lease->ntp, lease->ntp_count);
                        if (r < 0)
                                return r;

                        lease->ntp_count = r;

                        break;

                case DHCP6_NTP_SUBOPTION_SRV_FQDN: {
                        char **servers;

                        r = dhcp6_option_parse_domainname_list(subval, sublen, &servers);
                        if (r < 0)
                                return 0;

                        strv_free_and_replace(lease->ntp_fqdn, servers);
                        lease->ntp_fqdn_count = r;

                        break;
                }}
        }

        return 0;
}

int dhcp6_lease_set_sntp(sd_dhcp6_lease *lease, uint8_t *optval, size_t optlen) {
        int r;

        assert_return(lease, -EINVAL);
        assert_return(optval, -EINVAL);

        if (!optlen)
                return 0;

        if (lease->ntp || lease->ntp_fqdn)
                return -EEXIST;

        /* Using deprecated SNTP information */

        r = dhcp6_option_parse_ip6addrs(optval, optlen, &lease->ntp,
                                        lease->ntp_count);
        if (r < 0)
                return r;

        lease->ntp_count = r;

        return 0;
}

int sd_dhcp6_lease_get_ntp_addrs(sd_dhcp6_lease *lease,
                                 const struct in6_addr **addrs) {
        assert_return(lease, -EINVAL);
        assert_return(addrs, -EINVAL);

        if (lease->ntp_count) {
                *addrs = lease->ntp;
                return lease->ntp_count;
        }

        return -ENOENT;
}

int sd_dhcp6_lease_get_ntp_fqdn(sd_dhcp6_lease *lease, char ***ntp_fqdn) {
        assert_return(lease, -EINVAL);
        assert_return(ntp_fqdn, -EINVAL);

        if (lease->ntp_fqdn_count) {
                *ntp_fqdn = lease->ntp_fqdn;
                return lease->ntp_fqdn_count;
        }

        return -ENOENT;
}

int dhcp6_lease_set_fqdn(sd_dhcp6_lease *lease, const uint8_t *optval,
                         size_t optlen) {
        int r;
        char *fqdn;

        assert_return(lease, -EINVAL);
        assert_return(optval, -EINVAL);

        if (optlen < 2)
                return -ENODATA;

        /* Ignore the flags field, it doesn't carry any useful
           information for clients. */
        r = dhcp6_option_parse_domainname(optval + 1, optlen - 1, &fqdn);
        if (r < 0)
                return r;

        return free_and_replace(lease->fqdn, fqdn);
}

int sd_dhcp6_lease_get_fqdn(sd_dhcp6_lease *lease, const char **fqdn) {
        assert_return(lease, -EINVAL);
        assert_return(fqdn, -EINVAL);

        if (lease->fqdn) {
                *fqdn = lease->fqdn;
                return 0;
        }

        return -ENOENT;
}

static sd_dhcp6_lease *dhcp6_lease_free(sd_dhcp6_lease *lease) {
        assert(lease);

        free(lease->serverid);
        dhcp6_lease_free_ia(&lease->ia);
        dhcp6_lease_free_ia(&lease->pd);

        free(lease->dns);
        free(lease->fqdn);

        lease->domains = strv_free(lease->domains);

        free(lease->ntp);

        lease->ntp_fqdn = strv_free(lease->ntp_fqdn);
        return mfree(lease);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(sd_dhcp6_lease, sd_dhcp6_lease, dhcp6_lease_free);

int dhcp6_lease_new(sd_dhcp6_lease **ret) {
        sd_dhcp6_lease *lease;

        lease = new0(sd_dhcp6_lease, 1);
        if (!lease)
                return -ENOMEM;

        lease->n_ref = 1;

        LIST_HEAD_INIT(lease->ia.addresses);

        *ret = lease;
        return 0;
}
