/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  This file is part of systemd.

  Copyright 2014 Zbigniew Jędrzejewski-Szmek
***/

#include <getopt.h>
#include <net/if.h>

#include "sd-bus.h"
#include "sd-netlink.h"

#include "af-list.h"
#include "alloc-util.h"
#include "bus-common-errors.h"
#include "bus-error.h"
#include "bus-util.h"
#include "dns-domain.h"
#include "escape.h"
#include "gcrypt-util.h"
#include "in-addr-util.h"
#include "netlink-util.h"
#include "pager.h"
#include "parse-util.h"
#include "resolvconf-compat.h"
#include "resolvectl.h"
#include "resolved-def.h"
#include "resolved-dns-packet.h"
#include "string-table.h"
#include "strv.h"
#include "terminal-util.h"
#include "verbs.h"

static int arg_family = AF_UNSPEC;
int arg_ifindex = 0;
static uint16_t arg_type = 0;
static uint16_t arg_class = 0;
static bool arg_legend = true;
static uint64_t arg_flags = 0;
static bool arg_no_pager = false;
bool arg_ifindex_permissive = false; /* If true, don't generate an error if the specified interface index doesn't exist */

typedef enum ServiceFamily {
        SERVICE_FAMILY_TCP,
        SERVICE_FAMILY_UDP,
        SERVICE_FAMILY_SCTP,
        _SERVICE_FAMILY_MAX,
        _SERVICE_FAMILY_INVALID = -1,
} ServiceFamily;
static ServiceFamily arg_service_family = SERVICE_FAMILY_TCP;

typedef enum RawType {
        RAW_NONE,
        RAW_PAYLOAD,
        RAW_PACKET,
} RawType;
static RawType arg_raw = RAW_NONE;

ExecutionMode arg_mode = MODE_RESOLVE_HOST;

char **arg_set_dns = NULL;
char **arg_set_domain = NULL;
static const char *arg_set_llmnr = NULL;
static const char *arg_set_mdns = NULL;
static const char *arg_set_dnssec = NULL;
static char **arg_set_nta = NULL;

typedef enum StatusMode {
        STATUS_ALL,
        STATUS_DNS,
        STATUS_DOMAIN,
        STATUS_LLMNR,
        STATUS_MDNS,
        STATUS_DNSSEC,
        STATUS_NTA,
} StatusMode;

static int parse_ifindex_with_warn(const char *s) {
        int ifi;

        assert(s);

        if (parse_ifindex(s, &ifi) < 0) {
                ifi = if_nametoindex(s);
                if (ifi <= 0)
                        return log_error_errno(errno, "Unknown interface %s: %m", s);
        }

        return ifi;
}

static const char * const service_family_table[_SERVICE_FAMILY_MAX] = {
        [SERVICE_FAMILY_TCP]  = "tcp",
        [SERVICE_FAMILY_UDP]  = "udp",
        [SERVICE_FAMILY_SCTP] = "sctp",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP(service_family, ServiceFamily);

static void print_source(uint64_t flags, usec_t rtt) {
        char rtt_str[FORMAT_TIMESTAMP_MAX];

        if (!arg_legend)
                return;

        if (flags == 0)
                return;

        fputs("\n-- Information acquired via", stdout);

        if (flags != 0)
                printf(" protocol%s%s%s%s%s",
                       flags & SD_RESOLVED_DNS ? " DNS" :"",
                       flags & SD_RESOLVED_LLMNR_IPV4 ? " LLMNR/IPv4" : "",
                       flags & SD_RESOLVED_LLMNR_IPV6 ? " LLMNR/IPv6" : "",
                       flags & SD_RESOLVED_MDNS_IPV4 ? " mDNS/IPv4" : "",
                       flags & SD_RESOLVED_MDNS_IPV6 ? " mDNS/IPv6" : "");

        assert_se(format_timespan(rtt_str, sizeof(rtt_str), rtt, 100));

        printf(" in %s", rtt_str);

        fputc('.', stdout);
        fputc('\n', stdout);

        printf("-- Data is authenticated: %s\n", yes_no(flags & SD_RESOLVED_AUTHENTICATED));
}

static int resolve_host(sd_bus *bus, const char *name) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        const char *canonical = NULL;
        char ifname[IF_NAMESIZE] = "";
        unsigned c = 0;
        int r;
        uint64_t flags;
        usec_t ts;

        assert(name);

        if (arg_ifindex > 0 && !if_indextoname(arg_ifindex, ifname))
                return log_error_errno(errno, "Failed to resolve interface name for index %i: %m", arg_ifindex);

        log_debug("Resolving %s (family %s, interface %s).", name, af_to_name(arg_family) ?: "*", isempty(ifname) ? "*" : ifname);

        r = sd_bus_message_new_method_call(
                        bus,
                        &req,
                        "org.freedesktop.resolve1",
                        "/org/freedesktop/resolve1",
                        "org.freedesktop.resolve1.Manager",
                        "ResolveHostname");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "isit", arg_ifindex, name, arg_family, arg_flags);
        if (r < 0)
                return bus_log_create_error(r);

        ts = now(CLOCK_MONOTONIC);

        r = sd_bus_call(bus, req, SD_RESOLVED_QUERY_TIMEOUT_USEC, &error, &reply);
        if (r < 0)
                return log_error_errno(r, "%s: resolve call failed: %s", name, bus_error_message(&error, r));

        ts = now(CLOCK_MONOTONIC) - ts;

        r = sd_bus_message_enter_container(reply, 'a', "(iiay)");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_enter_container(reply, 'r', "iiay")) > 0) {
                _cleanup_free_ char *pretty = NULL;
                int ifindex, family;
                const void *a;
                size_t sz;

                assert_cc(sizeof(int) == sizeof(int32_t));

                r = sd_bus_message_read(reply, "ii", &ifindex, &family);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_read_array(reply, 'y', &a, &sz);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (!IN_SET(family, AF_INET, AF_INET6)) {
                        log_debug("%s: skipping entry with family %d (%s)", name, family, af_to_name(family) ?: "unknown");
                        continue;
                }

                if (sz != FAMILY_ADDRESS_SIZE(family)) {
                        log_error("%s: systemd-resolved returned address of invalid size %zu for family %s", name, sz, af_to_name(family) ?: "unknown");
                        return -EINVAL;
                }

                ifname[0] = 0;
                if (ifindex > 0 && !if_indextoname(ifindex, ifname))
                        log_warning_errno(errno, "Failed to resolve interface name for index %i: %m", ifindex);

                r = in_addr_ifindex_to_string(family, a, ifindex, &pretty);
                if (r < 0)
                        return log_error_errno(r, "Failed to print address for %s: %m", name);

                printf("%*s%s %s%s%s\n",
                       (int) strlen(name), c == 0 ? name : "", c == 0 ? ":" : " ",
                       pretty,
                       isempty(ifname) ? "" : "%", ifname);

                c++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(reply, "st", &canonical, &flags);
        if (r < 0)
                return bus_log_parse_error(r);

        if (!streq(name, canonical))
                printf("%*s%s (%s)\n",
                       (int) strlen(name), c == 0 ? name : "", c == 0 ? ":" : " ",
                       canonical);

        if (c == 0) {
                log_error("%s: no addresses found", name);
                return -ESRCH;
        }

        print_source(flags, ts);

        return 0;
}

static int resolve_address(sd_bus *bus, int family, const union in_addr_union *address, int ifindex) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_free_ char *pretty = NULL;
        char ifname[IF_NAMESIZE] = "";
        uint64_t flags;
        unsigned c = 0;
        usec_t ts;
        int r;

        assert(bus);
        assert(IN_SET(family, AF_INET, AF_INET6));
        assert(address);

        if (ifindex <= 0)
                ifindex = arg_ifindex;

        r = in_addr_ifindex_to_string(family, address, ifindex, &pretty);
        if (r < 0)
                return log_oom();

        if (ifindex > 0 && !if_indextoname(ifindex, ifname))
                return log_error_errno(errno, "Failed to resolve interface name for index %i: %m", ifindex);

        log_debug("Resolving %s%s%s.", pretty, isempty(ifname) ? "" : "%", ifname);

        r = sd_bus_message_new_method_call(
                        bus,
                        &req,
                        "org.freedesktop.resolve1",
                        "/org/freedesktop/resolve1",
                        "org.freedesktop.resolve1.Manager",
                        "ResolveAddress");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "ii", ifindex, family);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append_array(req, 'y', address, FAMILY_ADDRESS_SIZE(family));
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "t", arg_flags);
        if (r < 0)
                return bus_log_create_error(r);

        ts = now(CLOCK_MONOTONIC);

        r = sd_bus_call(bus, req, SD_RESOLVED_QUERY_TIMEOUT_USEC, &error, &reply);
        if (r < 0) {
                log_error("%s: resolve call failed: %s", pretty, bus_error_message(&error, r));
                return r;
        }

        ts = now(CLOCK_MONOTONIC) - ts;

        r = sd_bus_message_enter_container(reply, 'a', "(is)");
        if (r < 0)
                return bus_log_create_error(r);

        while ((r = sd_bus_message_enter_container(reply, 'r', "is")) > 0) {
                const char *n;

                assert_cc(sizeof(int) == sizeof(int32_t));

                r = sd_bus_message_read(reply, "is", &ifindex, &n);
                if (r < 0)
                        return r;

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return r;

                ifname[0] = 0;
                if (ifindex > 0 && !if_indextoname(ifindex, ifname))
                        log_warning_errno(errno, "Failed to resolve interface name for index %i: %m", ifindex);

                printf("%*s%*s%*s%s %s\n",
                       (int) strlen(pretty), c == 0 ? pretty : "",
                       isempty(ifname) ? 0 : 1, c > 0 || isempty(ifname) ? "" : "%",
                       (int) strlen(ifname), c == 0 ? ifname : "",
                       c == 0 ? ":" : " ",
                       n);

                c++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(reply, "t", &flags);
        if (r < 0)
                return bus_log_parse_error(r);

        if (c == 0) {
                log_error("%s: no names found", pretty);
                return -ESRCH;
        }

        print_source(flags, ts);

        return 0;
}

static int output_rr_packet(const void *d, size_t l, int ifindex) {
        _cleanup_(dns_resource_record_unrefp) DnsResourceRecord *rr = NULL;
        _cleanup_(dns_packet_unrefp) DnsPacket *p = NULL;
        int r;
        char ifname[IF_NAMESIZE] = "";

        r = dns_packet_new(&p, DNS_PROTOCOL_DNS, 0, DNS_PACKET_SIZE_MAX);
        if (r < 0)
                return log_oom();

        p->refuse_compression = true;

        r = dns_packet_append_blob(p, d, l, NULL);
        if (r < 0)
                return log_oom();

        r = dns_packet_read_rr(p, &rr, NULL, NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to parse RR: %m");

        if (arg_raw == RAW_PAYLOAD) {
                void *data;
                ssize_t k;

                k = dns_resource_record_payload(rr, &data);
                if (k < 0)
                        return log_error_errno(k, "Cannot dump RR: %m");
                fwrite(data, 1, k, stdout);
        } else {
                const char *s;

                s = dns_resource_record_to_string(rr);
                if (!s)
                        return log_oom();

                if (ifindex > 0 && !if_indextoname(ifindex, ifname))
                        log_warning_errno(errno, "Failed to resolve interface name for index %i: %m", ifindex);

                printf("%s%s%s\n", s, isempty(ifname) ? "" : " # interface ", ifname);
        }

        return 0;
}

static int resolve_record(sd_bus *bus, const char *name, uint16_t class, uint16_t type, bool warn_missing) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        char ifname[IF_NAMESIZE] = "";
        unsigned n = 0;
        uint64_t flags;
        int r;
        usec_t ts;
        bool needs_authentication = false;

        assert(name);

        if (arg_ifindex > 0 && !if_indextoname(arg_ifindex, ifname))
                return log_error_errno(errno, "Failed to resolve interface name for index %i: %m", arg_ifindex);

        log_debug("Resolving %s %s %s (interface %s).", name, dns_class_to_string(class), dns_type_to_string(type), isempty(ifname) ? "*" : ifname);

        r = sd_bus_message_new_method_call(
                        bus,
                        &req,
                        "org.freedesktop.resolve1",
                        "/org/freedesktop/resolve1",
                        "org.freedesktop.resolve1.Manager",
                        "ResolveRecord");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "isqqt", arg_ifindex, name, class, type, arg_flags);
        if (r < 0)
                return bus_log_create_error(r);

        ts = now(CLOCK_MONOTONIC);

        r = sd_bus_call(bus, req, SD_RESOLVED_QUERY_TIMEOUT_USEC, &error, &reply);
        if (r < 0) {
                if (warn_missing || r != -ENXIO)
                        log_error("%s: resolve call failed: %s", name, bus_error_message(&error, r));
                return r;
        }

        ts = now(CLOCK_MONOTONIC) - ts;

        r = sd_bus_message_enter_container(reply, 'a', "(iqqay)");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_enter_container(reply, 'r', "iqqay")) > 0) {
                uint16_t c, t;
                int ifindex;
                const void *d;
                size_t l;

                assert_cc(sizeof(int) == sizeof(int32_t));

                r = sd_bus_message_read(reply, "iqq", &ifindex, &c, &t);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_read_array(reply, 'y', &d, &l);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (arg_raw == RAW_PACKET) {
                        uint64_t u64 = htole64(l);

                        fwrite(&u64, sizeof(u64), 1, stdout);
                        fwrite(d, 1, l, stdout);
                } else {
                        r = output_rr_packet(d, l, ifindex);
                        if (r < 0)
                                return r;
                }

                if (dns_type_needs_authentication(t))
                        needs_authentication = true;

                n++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(reply, "t", &flags);
        if (r < 0)
                return bus_log_parse_error(r);

        if (n == 0) {
                if (warn_missing)
                        log_error("%s: no records found", name);
                return -ESRCH;
        }

        print_source(flags, ts);

        if ((flags & SD_RESOLVED_AUTHENTICATED) == 0 && needs_authentication) {
                fflush(stdout);

                fprintf(stderr, "\n%s"
                       "WARNING: The resources shown contain cryptographic key data which could not be\n"
                       "         authenticated. It is not suitable to authenticate any communication.\n"
                       "         This is usually indication that DNSSEC authentication was not enabled\n"
                       "         or is not available for the selected protocol or DNS servers.%s\n",
                       ansi_highlight_red(),
                       ansi_normal());
        }

        return 0;
}

static int resolve_rfc4501(sd_bus *bus, const char *name) {
        uint16_t type = 0, class = 0;
        const char *p, *q, *n;
        int r;

        assert(bus);
        assert(name);
        assert(startswith(name, "dns:"));

        /* Parse RFC 4501 dns: URIs */

        p = name + 4;

        if (p[0] == '/') {
                const char *e;

                if (p[1] != '/')
                        goto invalid;

                e = strchr(p + 2, '/');
                if (!e)
                        goto invalid;

                if (e != p + 2)
                        log_warning("DNS authority specification not supported; ignoring specified authority.");

                p = e + 1;
        }

        q = strchr(p, '?');
        if (q) {
                n = strndupa(p, q - p);
                q++;

                for (;;) {
                        const char *f;

                        f = startswith_no_case(q, "class=");
                        if (f) {
                                _cleanup_free_ char *t = NULL;
                                const char *e;

                                if (class != 0) {
                                        log_error("DNS class specified twice.");
                                        return -EINVAL;
                                }

                                e = strchrnul(f, ';');
                                t = strndup(f, e - f);
                                if (!t)
                                        return log_oom();

                                r = dns_class_from_string(t);
                                if (r < 0) {
                                        log_error("Unknown DNS class %s.", t);
                                        return -EINVAL;
                                }

                                class = r;

                                if (*e == ';') {
                                        q = e + 1;
                                        continue;
                                }

                                break;
                        }

                        f = startswith_no_case(q, "type=");
                        if (f) {
                                _cleanup_free_ char *t = NULL;
                                const char *e;

                                if (type != 0) {
                                        log_error("DNS type specified twice.");
                                        return -EINVAL;
                                }

                                e = strchrnul(f, ';');
                                t = strndup(f, e - f);
                                if (!t)
                                        return log_oom();

                                r = dns_type_from_string(t);
                                if (r < 0) {
                                        log_error("Unknown DNS type %s.", t);
                                        return -EINVAL;
                                }

                                type = r;

                                if (*e == ';') {
                                        q = e + 1;
                                        continue;
                                }

                                break;
                        }

                        goto invalid;
                }
        } else
                n = p;

        if (class == 0)
                class = arg_class ?: DNS_CLASS_IN;
        if (type == 0)
                type = arg_type ?: DNS_TYPE_A;

        return resolve_record(bus, n, class, type, true);

invalid:
        log_error("Invalid DNS URI: %s", name);
        return -EINVAL;
}

static int verb_query(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;
        char **p;
        int q, r = 0;

        if (arg_type != 0)
                STRV_FOREACH(p, argv + 1) {
                        q = resolve_record(bus, *p, arg_class, arg_type, true);
                        if (q < 0)
                                r = q;
                }

        else
                STRV_FOREACH(p, argv + 1) {
                        if (startswith(*p, "dns:"))
                                q = resolve_rfc4501(bus, *p);
                        else {
                                int family, ifindex;
                                union in_addr_union a;

                                q = in_addr_ifindex_from_string_auto(*p, &family, &a, &ifindex);
                                if (q >= 0)
                                        q = resolve_address(bus, family, &a, ifindex);
                                else
                                        q = resolve_host(bus, *p);
                        }
                        if (q < 0)
                                r = q;
                }

        return r;
}

static int resolve_service(sd_bus *bus, const char *name, const char *type, const char *domain) {
        const char *canonical_name, *canonical_type, *canonical_domain;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL, *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        char ifname[IF_NAMESIZE] = "";
        size_t indent, sz;
        uint64_t flags;
        const char *p;
        unsigned c;
        usec_t ts;
        int r;

        assert(bus);
        assert(domain);

        name = empty_to_null(name);
        type = empty_to_null(type);

        if (arg_ifindex > 0 && !if_indextoname(arg_ifindex, ifname))
                return log_error_errno(errno, "Failed to resolve interface name for index %i: %m", arg_ifindex);

        if (name)
                log_debug("Resolving service \"%s\" of type %s in %s (family %s, interface %s).", name, type, domain, af_to_name(arg_family) ?: "*", isempty(ifname) ? "*" : ifname);
        else if (type)
                log_debug("Resolving service type %s of %s (family %s, interface %s).", type, domain, af_to_name(arg_family) ?: "*", isempty(ifname) ? "*" : ifname);
        else
                log_debug("Resolving service type %s (family %s, interface %s).", domain, af_to_name(arg_family) ?: "*", isempty(ifname) ? "*" : ifname);

        r = sd_bus_message_new_method_call(
                        bus,
                        &req,
                        "org.freedesktop.resolve1",
                        "/org/freedesktop/resolve1",
                        "org.freedesktop.resolve1.Manager",
                        "ResolveService");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "isssit", arg_ifindex, name, type, domain, arg_family, arg_flags);
        if (r < 0)
                return bus_log_create_error(r);

        ts = now(CLOCK_MONOTONIC);

        r = sd_bus_call(bus, req, SD_RESOLVED_QUERY_TIMEOUT_USEC, &error, &reply);
        if (r < 0)
                return log_error_errno(r, "Resolve call failed: %s", bus_error_message(&error, r));

        ts = now(CLOCK_MONOTONIC) - ts;

        r = sd_bus_message_enter_container(reply, 'a', "(qqqsa(iiay)s)");
        if (r < 0)
                return bus_log_parse_error(r);

        indent =
                (name ? strlen(name) + 1 : 0) +
                (type ? strlen(type) + 1 : 0) +
                strlen(domain) + 2;

        c = 0;
        while ((r = sd_bus_message_enter_container(reply, 'r', "qqqsa(iiay)s")) > 0) {
                uint16_t priority, weight, port;
                const char *hostname, *canonical;

                r = sd_bus_message_read(reply, "qqqs", &priority, &weight, &port, &hostname);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (name)
                        printf("%*s%s", (int) strlen(name), c == 0 ? name : "", c == 0 ? "/" : " ");
                if (type)
                        printf("%*s%s", (int) strlen(type), c == 0 ? type : "", c == 0 ? "/" : " ");

                printf("%*s%s %s:%u [priority=%u, weight=%u]\n",
                       (int) strlen(domain), c == 0 ? domain : "",
                       c == 0 ? ":" : " ",
                       hostname, port,
                       priority, weight);

                r = sd_bus_message_enter_container(reply, 'a', "(iiay)");
                if (r < 0)
                        return bus_log_parse_error(r);

                while ((r = sd_bus_message_enter_container(reply, 'r', "iiay")) > 0) {
                        _cleanup_free_ char *pretty = NULL;
                        int ifindex, family;
                        const void *a;

                        assert_cc(sizeof(int) == sizeof(int32_t));

                        r = sd_bus_message_read(reply, "ii", &ifindex, &family);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_read_array(reply, 'y', &a, &sz);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        r = sd_bus_message_exit_container(reply);
                        if (r < 0)
                                return bus_log_parse_error(r);

                        if (!IN_SET(family, AF_INET, AF_INET6)) {
                                log_debug("%s: skipping entry with family %d (%s)", name, family, af_to_name(family) ?: "unknown");
                                continue;
                        }

                        if (sz != FAMILY_ADDRESS_SIZE(family)) {
                                log_error("%s: systemd-resolved returned address of invalid size %zu for family %s", name, sz, af_to_name(family) ?: "unknown");
                                return -EINVAL;
                        }

                        ifname[0] = 0;
                        if (ifindex > 0 && !if_indextoname(ifindex, ifname))
                                log_warning_errno(errno, "Failed to resolve interface name for index %i: %m", ifindex);

                        r = in_addr_to_string(family, a, &pretty);
                        if (r < 0)
                                return log_error_errno(r, "Failed to print address for %s: %m", name);

                        printf("%*s%s%s%s\n", (int) indent, "", pretty, isempty(ifname) ? "" : "%s", ifname);
                }
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

                r = sd_bus_message_read(reply, "s", &canonical);
                if (r < 0)
                        return bus_log_parse_error(r);

                if (!streq(hostname, canonical))
                        printf("%*s(%s)\n", (int) indent, "", canonical);

                r = sd_bus_message_exit_container(reply);
                if (r < 0)
                        return bus_log_parse_error(r);

                c++;
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_enter_container(reply, 'a', "ay");
        if (r < 0)
                return bus_log_parse_error(r);

        while ((r = sd_bus_message_read_array(reply, 'y', (const void**) &p, &sz)) > 0) {
                _cleanup_free_ char *escaped = NULL;

                escaped = cescape_length(p, sz);
                if (!escaped)
                        return log_oom();

                printf("%*s%s\n", (int) indent, "", escaped);
        }
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_exit_container(reply);
        if (r < 0)
                return bus_log_parse_error(r);

        r = sd_bus_message_read(reply, "ssst", &canonical_name, &canonical_type, &canonical_domain, &flags);
        if (r < 0)
                return bus_log_parse_error(r);

        canonical_name = empty_to_null(canonical_name);
        canonical_type = empty_to_null(canonical_type);

        if (!streq_ptr(name, canonical_name) ||
            !streq_ptr(type, canonical_type) ||
            !streq_ptr(domain, canonical_domain)) {

                printf("%*s(", (int) indent, "");

                if (canonical_name)
                        printf("%s/", canonical_name);
                if (canonical_type)
                        printf("%s/", canonical_type);

                printf("%s)\n", canonical_domain);
        }

        print_source(flags, ts);

        return 0;
}

static int verb_service(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;

        if (argc == 2)
                return resolve_service(bus, NULL, NULL, argv[1]);
        else if (argc == 3)
                return resolve_service(bus, NULL, argv[1], argv[2]);
        else
                return resolve_service(bus, argv[1], argv[2], argv[3]);
}

static int resolve_openpgp(sd_bus *bus, const char *address) {
        const char *domain, *full;
        int r;
        _cleanup_free_ char *hashed = NULL;

        assert(bus);
        assert(address);

        domain = strrchr(address, '@');
        if (!domain) {
                log_error("Address does not contain '@': \"%s\"", address);
                return -EINVAL;
        } else if (domain == address || domain[1] == '\0') {
                log_error("Address starts or ends with '@': \"%s\"", address);
                return -EINVAL;
        }
        domain++;

        r = string_hashsum_sha256(address, domain - 1 - address, &hashed);
        if (r < 0)
                return log_error_errno(r, "Hashing failed: %m");

        strshorten(hashed, 56);

        full = strjoina(hashed, "._openpgpkey.", domain);
        log_debug("Looking up \"%s\".", full);

        r = resolve_record(bus, full,
                           arg_class ?: DNS_CLASS_IN,
                           arg_type ?: DNS_TYPE_OPENPGPKEY, false);

        if (IN_SET(r, -ENXIO, -ESRCH)) { /* NXDOMAIN or NODATA? */
              hashed = NULL;
              r = string_hashsum_sha224(address, domain - 1 - address, &hashed);
              if (r < 0)
                    return log_error_errno(r, "Hashing failed: %m");

              full = strjoina(hashed, "._openpgpkey.", domain);
              log_debug("Looking up \"%s\".", full);

              return resolve_record(bus, full,
                                    arg_class ?: DNS_CLASS_IN,
                                    arg_type ?: DNS_TYPE_OPENPGPKEY, true);
        }

        return r;
}

static int verb_openpgp(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;
        char **p;
        int q, r = 0;

        STRV_FOREACH(p, argv + 1) {
                q = resolve_openpgp(bus, *p);
                if (q < 0)
                        r = q;
        }

        return r;
}

static int resolve_tlsa(sd_bus *bus, const char *address) {
        const char *port;
        uint16_t port_num = 443;
        _cleanup_free_ char *full = NULL;
        int r;

        assert(bus);
        assert(address);

        port = strrchr(address, ':');
        if (port) {
                r = parse_ip_port(port + 1, &port_num);
                if (r < 0)
                        return log_error_errno(r, "Invalid port \"%s\".", port + 1);

                address = strndupa(address, port - address);
        }

        r = asprintf(&full, "_%u._%s.%s",
                     port_num,
                     service_family_to_string(arg_service_family),
                     address);
        if (r < 0)
                return log_oom();

        log_debug("Looking up \"%s\".", full);

        return resolve_record(bus, full,
                              arg_class ?: DNS_CLASS_IN,
                              arg_type ?: DNS_TYPE_TLSA, true);
}

static int verb_tlsa(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;
        char **p;
        int q, r = 0;

        STRV_FOREACH(p, argv + 1) {
                q = resolve_tlsa(bus, *p);
                if (q < 0)
                        r = q;
        }

        return r;
}

static int show_statistics(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        sd_bus *bus = userdata;
        uint64_t n_current_transactions, n_total_transactions,
                cache_size, n_cache_hit, n_cache_miss,
                n_dnssec_secure, n_dnssec_insecure, n_dnssec_bogus, n_dnssec_indeterminate;
        int r, dnssec_supported;

        assert(bus);

        r = sd_bus_get_property_trivial(bus,
                                        "org.freedesktop.resolve1",
                                        "/org/freedesktop/resolve1",
                                        "org.freedesktop.resolve1.Manager",
                                        "DNSSECSupported",
                                        &error,
                                        'b',
                                        &dnssec_supported);
        if (r < 0)
                return log_error_errno(r, "Failed to get DNSSEC supported state: %s", bus_error_message(&error, r));

        printf("DNSSEC supported by current servers: %s%s%s\n\n",
               ansi_highlight(),
               yes_no(dnssec_supported),
               ansi_normal());

        r = sd_bus_get_property(bus,
                                "org.freedesktop.resolve1",
                                "/org/freedesktop/resolve1",
                                "org.freedesktop.resolve1.Manager",
                                "TransactionStatistics",
                                &error,
                                &reply,
                                "(tt)");
        if (r < 0)
                return log_error_errno(r, "Failed to get transaction statistics: %s", bus_error_message(&error, r));

        r = sd_bus_message_read(reply, "(tt)",
                                &n_current_transactions,
                                &n_total_transactions);
        if (r < 0)
                return bus_log_parse_error(r);

        printf("%sTransactions%s\n"
               "Current Transactions: %" PRIu64 "\n"
               "  Total Transactions: %" PRIu64 "\n",
               ansi_highlight(),
               ansi_normal(),
               n_current_transactions,
               n_total_transactions);

        reply = sd_bus_message_unref(reply);

        r = sd_bus_get_property(bus,
                                "org.freedesktop.resolve1",
                                "/org/freedesktop/resolve1",
                                "org.freedesktop.resolve1.Manager",
                                "CacheStatistics",
                                &error,
                                &reply,
                                "(ttt)");
        if (r < 0)
                return log_error_errno(r, "Failed to get cache statistics: %s", bus_error_message(&error, r));

        r = sd_bus_message_read(reply, "(ttt)",
                                &cache_size,
                                &n_cache_hit,
                                &n_cache_miss);
        if (r < 0)
                return bus_log_parse_error(r);

        printf("\n%sCache%s\n"
               "  Current Cache Size: %" PRIu64 "\n"
               "          Cache Hits: %" PRIu64 "\n"
               "        Cache Misses: %" PRIu64 "\n",
               ansi_highlight(),
               ansi_normal(),
               cache_size,
               n_cache_hit,
               n_cache_miss);

        reply = sd_bus_message_unref(reply);

        r = sd_bus_get_property(bus,
                                "org.freedesktop.resolve1",
                                "/org/freedesktop/resolve1",
                                "org.freedesktop.resolve1.Manager",
                                "DNSSECStatistics",
                                &error,
                                &reply,
                                "(tttt)");
        if (r < 0)
                return log_error_errno(r, "Failed to get DNSSEC statistics: %s", bus_error_message(&error, r));

        r = sd_bus_message_read(reply, "(tttt)",
                                &n_dnssec_secure,
                                &n_dnssec_insecure,
                                &n_dnssec_bogus,
                                &n_dnssec_indeterminate);
        if (r < 0)
                return bus_log_parse_error(r);

        printf("\n%sDNSSEC Verdicts%s\n"
               "              Secure: %" PRIu64 "\n"
               "            Insecure: %" PRIu64 "\n"
               "               Bogus: %" PRIu64 "\n"
               "       Indeterminate: %" PRIu64 "\n",
               ansi_highlight(),
               ansi_normal(),
               n_dnssec_secure,
               n_dnssec_insecure,
               n_dnssec_bogus,
               n_dnssec_indeterminate);

        return 0;
}

static int reset_statistics(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        r = sd_bus_call_method(bus,
                               "org.freedesktop.resolve1",
                               "/org/freedesktop/resolve1",
                               "org.freedesktop.resolve1.Manager",
                               "ResetStatistics",
                               &error,
                               NULL,
                               NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to reset statistics: %s", bus_error_message(&error, r));

        return 0;
}

static int flush_caches(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        r = sd_bus_call_method(bus,
                               "org.freedesktop.resolve1",
                               "/org/freedesktop/resolve1",
                               "org.freedesktop.resolve1.Manager",
                               "FlushCaches",
                               &error,
                               NULL,
                               NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to flush caches: %s", bus_error_message(&error, r));

        return 0;
}

static int reset_server_features(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        r = sd_bus_call_method(bus,
                               "org.freedesktop.resolve1",
                               "/org/freedesktop/resolve1",
                               "org.freedesktop.resolve1.Manager",
                               "ResetServerFeatures",
                               &error,
                               NULL,
                               NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to reset server features: %s", bus_error_message(&error, r));

        return 0;
}

static int read_dns_server_one(sd_bus_message *m, bool with_ifindex, char **ret) {
        _cleanup_free_ char *pretty = NULL;
        int ifindex, family, r;
        const void *a;
        size_t sz;

        assert(m);
        assert(ret);

        r = sd_bus_message_enter_container(m, 'r', with_ifindex ? "iiay" : "iay");
        if (r <= 0)
                return r;

        if (with_ifindex) {
                r = sd_bus_message_read(m, "i", &ifindex);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_read(m, "i", &family);
        if (r < 0)
                return r;

        r = sd_bus_message_read_array(m, 'y', &a, &sz);
        if (r < 0)
                return r;

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        if (with_ifindex && ifindex != 0) {
                /* only show the global ones here */
                *ret = NULL;
                return 1;
        }

        if (!IN_SET(family, AF_INET, AF_INET6)) {
                log_debug("Unexpected family, ignoring: %i", family);

                *ret = NULL;
                return 1;
        }

        if (sz != FAMILY_ADDRESS_SIZE(family)) {
                log_debug("Address size mismatch, ignoring.");

                *ret = NULL;
                return 1;
        }

        r = in_addr_to_string(family, a, &pretty);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(pretty);

        return 1;
}

static int map_link_dns_servers(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        char ***l = userdata;
        int r;

        assert(bus);
        assert(member);
        assert(m);
        assert(l);

        r = sd_bus_message_enter_container(m, 'a', "(iay)");
        if (r < 0)
                return r;

        for (;;) {
                char *pretty = NULL;

                r = read_dns_server_one(m, false, &pretty);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (isempty(pretty))
                        continue;

                r = strv_consume(l, pretty);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        return 0;
}

static int map_link_current_dns_server(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        assert(m);
        assert(userdata);

        return read_dns_server_one(m, false, userdata);
}

static int read_domain_one(sd_bus_message *m, bool with_ifindex, char **ret) {
        _cleanup_free_ char *str = NULL;
        int ifindex, route_only, r;
        const char *domain;

        assert(m);
        assert(ret);

        if (with_ifindex)
                r = sd_bus_message_read(m, "(isb)", &ifindex, &domain, &route_only);
        else
                r = sd_bus_message_read(m, "(sb)", &domain, &route_only);
        if (r <= 0)
                return r;

        if (with_ifindex && ifindex != 0) {
                /* only show the global ones here */
                *ret = NULL;
                return 1;
        }

        if (route_only)
                str = strappend("~", domain);
        else
                str = strdup(domain);
        if (!str)
                return -ENOMEM;

        *ret = TAKE_PTR(str);

        return 1;
}

static int map_link_domains(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        char ***l = userdata;
        int r;

        assert(bus);
        assert(member);
        assert(m);
        assert(l);

        r = sd_bus_message_enter_container(m, 'a', "(sb)");
        if (r < 0)
                return r;

        for (;;) {
                char *pretty = NULL;

                r = read_domain_one(m, false, &pretty);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (isempty(pretty))
                        continue;

                r = strv_consume(l, pretty);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        return 0;
}

static int status_print_strv_ifindex(int ifindex, const char *ifname, char **p) {
        char **i;

        printf("%sLink %i (%s)%s:",
               ansi_highlight(), ifindex, ifname, ansi_normal());

        STRV_FOREACH(i, p)
                printf(" %s", *i);

        printf("\n");

        return 0;
}

static int status_ifindex(sd_bus *bus, int ifindex, const char *name, StatusMode mode, bool *empty_line) {

        struct link_info {
                uint64_t scopes_mask;
                const char *llmnr;
                const char *mdns;
                const char *dnssec;
                char *current_dns;
                char **dns;
                char **domains;
                char **ntas;
                bool dnssec_supported;
        } link_info = {};

        static const struct bus_properties_map property_map[] = {
                { "ScopesMask",                 "t",      NULL,                        offsetof(struct link_info, scopes_mask)      },
                { "DNS",                        "a(iay)", map_link_dns_servers,        offsetof(struct link_info, dns)              },
                { "CurrentDNSServer",           "(iay)",  map_link_current_dns_server, offsetof(struct link_info, current_dns)      },
                { "Domains",                    "a(sb)",  map_link_domains,            offsetof(struct link_info, domains)          },
                { "LLMNR",                      "s",      NULL,                        offsetof(struct link_info, llmnr)            },
                { "MulticastDNS",               "s",      NULL,                        offsetof(struct link_info, mdns)             },
                { "DNSSEC",                     "s",      NULL,                        offsetof(struct link_info, dnssec)           },
                { "DNSSECNegativeTrustAnchors", "as",     NULL,                        offsetof(struct link_info, ntas)             },
                { "DNSSECSupported",            "b",      NULL,                        offsetof(struct link_info, dnssec_supported) },
                {}
        };

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        _cleanup_free_ char *ifi = NULL, *p = NULL;
        char ifname[IF_NAMESIZE] = "";
        char **i;
        int r;

        assert(bus);
        assert(ifindex > 0);

        if (!name) {
                if (!if_indextoname(ifindex, ifname))
                        return log_error_errno(errno, "Failed to resolve interface name for %i: %m", ifindex);

                name = ifname;
        }

        if (asprintf(&ifi, "%i", ifindex) < 0)
                return log_oom();

        r = sd_bus_path_encode("/org/freedesktop/resolve1/link", ifi, &p);
        if (r < 0)
                return log_oom();

        r = bus_map_all_properties(bus,
                                   "org.freedesktop.resolve1",
                                   p,
                                   property_map,
                                   BUS_MAP_BOOLEAN_AS_BOOL,
                                   &error,
                                   &m,
                                   &link_info);
        if (r < 0) {
                log_error_errno(r, "Failed to get link data for %i: %s", ifindex, bus_error_message(&error, r));
                goto finish;
        }

        (void) pager_open(arg_no_pager, false);

        if (mode == STATUS_DNS) {
                r = status_print_strv_ifindex(ifindex, name, link_info.dns);
                goto finish;
        }

        if (mode == STATUS_DOMAIN) {
                r = status_print_strv_ifindex(ifindex, name, link_info.domains);
                goto finish;
        }

        if (mode == STATUS_NTA) {
                r = status_print_strv_ifindex(ifindex, name, link_info.ntas);
                goto finish;
        }

        if (mode == STATUS_LLMNR) {
                printf("%sLink %i (%s)%s: %s\n",
                       ansi_highlight(), ifindex, name, ansi_normal(),
                       strna(link_info.llmnr));

                r = 0;
                goto finish;
        }

        if (mode == STATUS_MDNS) {
                printf("%sLink %i (%s)%s: %s\n",
                       ansi_highlight(), ifindex, name, ansi_normal(),
                       strna(link_info.mdns));

                r = 0;
                goto finish;
        }

        if (mode == STATUS_DNSSEC) {
                printf("%sLink %i (%s)%s: %s\n",
                       ansi_highlight(), ifindex, name, ansi_normal(),
                       strna(link_info.dnssec));

                r = 0;
                goto finish;
        }

        if (empty_line && *empty_line)
                fputc('\n', stdout);

        printf("%sLink %i (%s)%s\n",
               ansi_highlight(), ifindex, name, ansi_normal());

        if (link_info.scopes_mask == 0)
                printf("      Current Scopes: none\n");
        else
                printf("      Current Scopes:%s%s%s%s%s\n",
                       link_info.scopes_mask & SD_RESOLVED_DNS ? " DNS" : "",
                       link_info.scopes_mask & SD_RESOLVED_LLMNR_IPV4 ? " LLMNR/IPv4" : "",
                       link_info.scopes_mask & SD_RESOLVED_LLMNR_IPV6 ? " LLMNR/IPv6" : "",
                       link_info.scopes_mask & SD_RESOLVED_MDNS_IPV4 ? " mDNS/IPv4" : "",
                       link_info.scopes_mask & SD_RESOLVED_MDNS_IPV6 ? " mDNS/IPv6" : "");

        printf("       LLMNR setting: %s\n"
               "MulticastDNS setting: %s\n"
               "      DNSSEC setting: %s\n"
               "    DNSSEC supported: %s\n"
               "  Current DNS Server: %s\n",
               strna(link_info.llmnr),
               strna(link_info.mdns),
               strna(link_info.dnssec),
               yes_no(link_info.dnssec_supported),
               strna(link_info.current_dns));

        STRV_FOREACH(i, link_info.dns) {
                printf("         %s %s\n",
                       i == link_info.dns ? "DNS Servers:" : "            ",
                       *i);
        }

        STRV_FOREACH(i, link_info.domains) {
                printf("          %s %s\n",
                       i == link_info.domains ? "DNS Domain:" : "           ",
                       *i);
        }

        STRV_FOREACH(i, link_info.ntas) {
                printf("          %s %s\n",
                       i == link_info.ntas ? "DNSSEC NTA:" : "           ",
                       *i);
        }

        if (empty_line)
                *empty_line = true;

        r = 0;

finish:
        free(link_info.current_dns);
        strv_free(link_info.dns);
        strv_free(link_info.domains);
        strv_free(link_info.ntas);
        return r;
}

static int map_global_dns_servers(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        char ***l = userdata;
        int r;

        assert(bus);
        assert(member);
        assert(m);
        assert(l);

        r = sd_bus_message_enter_container(m, 'a', "(iiay)");
        if (r < 0)
                return r;

        for (;;) {
                char *pretty = NULL;

                r = read_dns_server_one(m, true, &pretty);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (isempty(pretty))
                        continue;

                r = strv_consume(l, pretty);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        return 0;
}

static int map_global_current_dns_server(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        assert(m);
        assert(userdata);

        return read_dns_server_one(m, true, userdata);
}

static int map_global_domains(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        char ***l = userdata;
        int r;

        assert(bus);
        assert(member);
        assert(m);
        assert(l);

        r = sd_bus_message_enter_container(m, 'a', "(isb)");
        if (r < 0)
                return r;

        for (;;) {
                char *pretty = NULL;

                r = read_domain_one(m, true, &pretty);
                if (r < 0)
                        return r;
                if (r == 0)
                        break;

                if (isempty(pretty))
                        continue;

                r = strv_consume(l, pretty);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        return 0;
}

static int status_print_strv_global(char **p) {
        char **i;

        printf("%sGlobal%s:", ansi_highlight(), ansi_normal());

        STRV_FOREACH(i, p)
                printf(" %s", *i);

        printf("\n");

        return 0;
}

static int status_global(sd_bus *bus, StatusMode mode, bool *empty_line) {

        struct global_info {
                char *current_dns;
                char **dns;
                char **domains;
                char **ntas;
                const char *llmnr;
                const char *mdns;
                const char *dnssec;
                bool dnssec_supported;
        } global_info = {};

        static const struct bus_properties_map property_map[] = {
                { "DNS",                        "a(iiay)", map_global_dns_servers,        offsetof(struct global_info, dns)              },
                { "CurrentDNSServer",           "(iiay)",  map_global_current_dns_server, offsetof(struct global_info, current_dns)      },
                { "Domains",                    "a(isb)",  map_global_domains,            offsetof(struct global_info, domains)          },
                { "DNSSECNegativeTrustAnchors", "as",      NULL,                          offsetof(struct global_info, ntas)             },
                { "LLMNR",                      "s",       NULL,                          offsetof(struct global_info, llmnr)            },
                { "MulticastDNS",               "s",       NULL,                          offsetof(struct global_info, mdns)             },
                { "DNSSEC",                     "s",       NULL,                          offsetof(struct global_info, dnssec)           },
                { "DNSSECSupported",            "b",       NULL,                          offsetof(struct global_info, dnssec_supported) },
                {}
        };

        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        char **i;
        int r;

        assert(bus);
        assert(empty_line);

        r = bus_map_all_properties(bus,
                                   "org.freedesktop.resolve1",
                                   "/org/freedesktop/resolve1",
                                   property_map,
                                   BUS_MAP_BOOLEAN_AS_BOOL,
                                   &error,
                                   &m,
                                   &global_info);
        if (r < 0) {
                log_error_errno(r, "Failed to get global data: %s", bus_error_message(&error, r));
                goto finish;
        }

        (void) pager_open(arg_no_pager, false);

        if (mode == STATUS_DNS) {
                r = status_print_strv_global(global_info.dns);
                goto finish;
        }

        if (mode == STATUS_DOMAIN) {
                r = status_print_strv_global(global_info.domains);
                goto finish;
        }

        if (mode == STATUS_NTA) {
                r = status_print_strv_global(global_info.ntas);
                goto finish;
        }

        if (mode == STATUS_LLMNR) {
                printf("%sGlobal%s: %s\n", ansi_highlight(), ansi_normal(),
                       strna(global_info.llmnr));

                r = 0;
                goto finish;
        }

        if (mode == STATUS_MDNS) {
                printf("%sGlobal%s: %s\n", ansi_highlight(), ansi_normal(),
                       strna(global_info.mdns));

                r = 0;
                goto finish;
        }

        if (mode == STATUS_DNSSEC) {
                printf("%sGlobal%s: %s\n", ansi_highlight(), ansi_normal(),
                       strna(global_info.dnssec));

                r = 0;
                goto finish;
        }

        printf("%sGlobal%s\n", ansi_highlight(), ansi_normal());

        printf("       LLMNR setting: %s\n"
               "MulticastDNS setting: %s\n"
               "      DNSSEC setting: %s\n"
               "    DNSSEC supported: %s\n"
               "  Current DNS Server: %s\n",
               strna(global_info.llmnr),
               strna(global_info.mdns),
               strna(global_info.dnssec),
               yes_no(global_info.dnssec_supported),
               strna(global_info.current_dns));

        STRV_FOREACH(i, global_info.dns) {
                printf("         %s %s\n",
                       i == global_info.dns ? "DNS Servers:" : "            ",
                       *i);
        }

        STRV_FOREACH(i, global_info.domains) {
                printf("          %s %s\n",
                       i == global_info.domains ? "DNS Domain:" : "           ",
                       *i);
        }

        strv_sort(global_info.ntas);
        STRV_FOREACH(i, global_info.ntas) {
                printf("          %s %s\n",
                       i == global_info.ntas ? "DNSSEC NTA:" : "           ",
                       *i);
        }

        *empty_line = true;

        r = 0;

finish:
        free(global_info.current_dns);
        strv_free(global_info.dns);
        strv_free(global_info.domains);
        strv_free(global_info.ntas);

        return r;
}

static int status_all(sd_bus *bus, StatusMode mode) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL, *reply = NULL;
        _cleanup_(sd_netlink_unrefp) sd_netlink *rtnl = NULL;
        sd_netlink_message *i;
        bool empty_line = false;
        int r;

        assert(bus);

        r = status_global(bus, mode, &empty_line);
        if (r < 0)
                return r;

        r = sd_netlink_open(&rtnl);
        if (r < 0)
                return log_error_errno(r, "Failed to connect to netlink: %m");

        r = sd_rtnl_message_new_link(rtnl, &req, RTM_GETLINK, 0);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_message_request_dump(req, true);
        if (r < 0)
                return rtnl_log_create_error(r);

        r = sd_netlink_call(rtnl, req, 0, &reply);
        if (r < 0)
                return log_error_errno(r, "Failed to enumerate links: %m");

        r = 0;
        for (i = reply; i; i = sd_netlink_message_next(i)) {
                const char *name;
                int ifindex, q;
                uint16_t type;

                q = sd_netlink_message_get_type(i, &type);
                if (q < 0)
                        return rtnl_log_parse_error(q);

                if (type != RTM_NEWLINK)
                        continue;

                q = sd_rtnl_message_link_get_ifindex(i, &ifindex);
                if (q < 0)
                        return rtnl_log_parse_error(q);

                if (ifindex == LOOPBACK_IFINDEX)
                        continue;

                q = sd_netlink_message_read_string(i, IFLA_IFNAME, &name);
                if (q < 0)
                        return rtnl_log_parse_error(q);

                q = status_ifindex(bus, ifindex, name, mode, &empty_line);
                if (q < 0 && r >= 0)
                        r = q;
        }

        return r;
}

static int verb_status(int argc, char **argv, void *userdata) {
        sd_bus *bus = userdata;
        int q, r = 0;

        if (argc > 1) {
                char **ifname;
                bool empty_line = false;

                STRV_FOREACH(ifname, argv + 1) {
                        int ifindex;

                        ifindex = parse_ifindex_with_warn(*ifname);
                        if (ifindex < 0)
                                continue;

                        q = status_ifindex(bus, ifindex, NULL, STATUS_ALL, &empty_line);
                        if (q < 0)
                                r = q;
                }
        } else
                r = status_all(bus, STATUS_ALL);

        return r;
}

static int log_interface_is_managed(int r, int ifindex) {
        char ifname[IFNAMSIZ];

        return log_error_errno(r,
                               "The specified interface %s is managed by systemd-networkd. Operation refused.\n"
                               "Please configure DNS settings for systemd-networkd managed interfaces directly in their .network files.",
                               strna(if_indextoname(ifindex, ifname)));
}

static int verb_dns(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL;
        sd_bus *bus = userdata;
        char **p;
        int r;

        assert(bus);

        if (arg_ifindex == LOOPBACK_IFINDEX) {
                log_error("Interface can't be the loopback interface (lo). Sorry.");
                return -EINVAL;
        }

        if (argc <= 1) {
                if (arg_ifindex <= 0)
                        return status_all(bus, STATUS_DNS);
                else
                        return status_ifindex(bus, arg_ifindex, NULL, STATUS_DNS, NULL);
        }

        if (arg_ifindex <= 0) {
                log_error("--interface= option is required.");
                return -EINVAL;
        }

        r = sd_bus_message_new_method_call(
                        bus,
                        &req,
                        "org.freedesktop.resolve1",
                        "/org/freedesktop/resolve1",
                        "org.freedesktop.resolve1.Manager",
                        "SetLinkDNS");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "i", arg_ifindex);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_open_container(req, 'a', "(iay)");
        if (r < 0)
                return bus_log_create_error(r);

        STRV_FOREACH(p, argv + 1) {
                struct in_addr_data data;

                r = in_addr_from_string_auto(*p, &data.family, &data.address);
                if (r < 0)
                        return log_error_errno(r, "Failed to parse DNS server address: %s", *p);

                r = sd_bus_message_open_container(req, 'r', "iay");
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append(req, "i", data.family);
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_append_array(req, 'y', &data.address, FAMILY_ADDRESS_SIZE(data.family));
                if (r < 0)
                        return bus_log_create_error(r);

                r = sd_bus_message_close_container(req);
                if (r < 0)
                        return bus_log_create_error(r);
        }

        r = sd_bus_message_close_container(req);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_call(bus, req, 0, &error, NULL);
        if (r < 0) {
                if (sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY))
                        return log_interface_is_managed(r, arg_ifindex);

                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set DNS configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_domain(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL;
        sd_bus *bus = userdata;
        char **p;
        int r;

        assert(bus);

        if (arg_ifindex == LOOPBACK_IFINDEX) {
                log_error("Interface can't be the loopback interface (lo). Sorry.");
                return -EINVAL;
        }

        if (argc <= 1) {
                if (arg_ifindex <= 0)
                        return status_all(bus, STATUS_DOMAIN);
                else
                        return status_ifindex(bus, arg_ifindex, NULL, STATUS_DOMAIN, NULL);
        }

        if (arg_ifindex <= 0) {
                log_error("--interface= option is required.");
                return -EINVAL;
        }

        r = sd_bus_message_new_method_call(
                        bus,
                        &req,
                        "org.freedesktop.resolve1",
                        "/org/freedesktop/resolve1",
                        "org.freedesktop.resolve1.Manager",
                        "SetLinkDomains");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "i", arg_ifindex);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_open_container(req, 'a', "(sb)");
        if (r < 0)
                return bus_log_create_error(r);

        STRV_FOREACH(p, argv + 1) {
                const char *n;

                n = **p == '~' ? *p + 1 : *p;

                r = dns_name_is_valid(n);
                if (r < 0)
                        return log_error_errno(r, "Failed to validate specified domain %s: %m", n);
                if (r == 0) {
                        log_error("Domain not valid: %s", n);
                        return -EINVAL;
                }

                r = sd_bus_message_append(req, "(sb)", n, **p == '~');
                if (r < 0)
                        return bus_log_create_error(r);
        }

        r = sd_bus_message_close_container(req);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_call(bus, req, 0, &error, NULL);
        if (r < 0) {
                if (sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY))
                        return log_interface_is_managed(r, arg_ifindex);

                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set domain configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_llmnr(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (arg_ifindex == LOOPBACK_IFINDEX) {
                log_error("Interface can't be the loopback interface (lo). Sorry.");
                return -EINVAL;
        }

        if (argc <= 1) {
                if (arg_ifindex <= 0)
                        return status_all(bus, STATUS_LLMNR);
                else
                        return status_ifindex(bus, arg_ifindex, NULL, STATUS_LLMNR, NULL);
        }

        if (arg_ifindex <= 0) {
                log_error("--interface= option is required.");
                return -EINVAL;
        }

        r = sd_bus_call_method(bus,
                               "org.freedesktop.resolve1",
                               "/org/freedesktop/resolve1",
                               "org.freedesktop.resolve1.Manager",
                               "SetLinkLLMNR",
                               &error,
                               NULL,
                               "is", arg_ifindex, argv[1]);
        if (r < 0) {
                if (sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY))
                        return log_interface_is_managed(r, arg_ifindex);

                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set LLMNR configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_mdns(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (arg_ifindex == LOOPBACK_IFINDEX) {
                log_error("Interface can't be the loopback interface (lo). Sorry.");
                return -EINVAL;
        }

        if (argc <= 1) {
                if (arg_ifindex <= 0)
                        return status_all(bus, STATUS_MDNS);
                else
                        return status_ifindex(bus, arg_ifindex, NULL, STATUS_MDNS, NULL);
        }

        if (arg_ifindex <= 0) {
                log_error("--interface= option is required.");
                return -EINVAL;
        }

        r = sd_bus_call_method(bus,
                               "org.freedesktop.resolve1",
                               "/org/freedesktop/resolve1",
                               "org.freedesktop.resolve1.Manager",
                               "SetLinkMulticastDNS",
                               &error,
                               NULL,
                               "is", arg_ifindex, argv[1]);
        if (r < 0) {
                if (sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY))
                        return log_interface_is_managed(r, arg_ifindex);

                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set MulticastDNS configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_dnssec(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (arg_ifindex == LOOPBACK_IFINDEX) {
                log_error("Interface can't be the loopback interface (lo). Sorry.");
                return -EINVAL;
        }

        if (argc <= 1) {
                if (arg_ifindex <= 0)
                        return status_all(bus, STATUS_DNSSEC);
                else
                        return status_ifindex(bus, arg_ifindex, NULL, STATUS_DNSSEC, NULL);
        }

        if (arg_ifindex <= 0) {
                log_error("--interface= option is required.");
                return -EINVAL;
        }

        r = sd_bus_call_method(bus,
                               "org.freedesktop.resolve1",
                               "/org/freedesktop/resolve1",
                               "org.freedesktop.resolve1.Manager",
                               "SetLinkDNSSEC",
                               &error,
                               NULL,
                               "is", arg_ifindex, argv[1]);
        if (r < 0) {
                if (sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY))
                        return log_interface_is_managed(r, arg_ifindex);

                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set DNSSEC configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_nta(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *req = NULL;
        sd_bus *bus = userdata;
        int i, r;

        assert(bus);

        if (arg_ifindex == LOOPBACK_IFINDEX) {
                log_error("Interface can't be the loopback interface (lo). Sorry.");
                return -EINVAL;
        }

        if (argc <= 1) {
                if (arg_ifindex <= 0)
                        return status_all(bus, STATUS_NTA);
                else
                        return status_ifindex(bus, arg_ifindex, NULL, STATUS_NTA, NULL);
        }

        if (arg_ifindex <= 0) {
                log_error("--interface= option is required.");
                return -EINVAL;
        }

        for (i = 1; i < argc; i++) {
                r = dns_name_is_valid(argv[i]);
                if (r < 0)
                        return log_error_errno(r, "Failed to validate specified domain %s: %m", argv[i]);
                if (r == 0) {
                        log_error("Domain not valid: %s", argv[i]);
                        return -EINVAL;
                }
        }

        r = sd_bus_message_new_method_call(
                        bus,
                        &req,
                        "org.freedesktop.resolve1",
                        "/org/freedesktop/resolve1",
                        "org.freedesktop.resolve1.Manager",
                        "SetLinkDNSSECNegativeTrustAnchors");
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append(req, "i", arg_ifindex);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_message_append_strv(req, argv + 1);
        if (r < 0)
                return bus_log_create_error(r);

        r = sd_bus_call(bus, req, 0, &error, NULL);
        if (r < 0) {
                if (sd_bus_error_has_name(&error, BUS_ERROR_LINK_BUSY))
                        return log_interface_is_managed(r, arg_ifindex);

                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to set DNSSEC NTA configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static int verb_revert_link(int argc, char **argv, void *userdata) {
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus *bus = userdata;
        int r;

        assert(bus);

        if (arg_ifindex == LOOPBACK_IFINDEX) {
                log_error("Interface can't be the loopback interface (lo). Sorry.");
                return -EINVAL;
        }

        r = sd_bus_call_method(bus,
                               "org.freedesktop.resolve1",
                               "/org/freedesktop/resolve1",
                               "org.freedesktop.resolve1.Manager",
                               "RevertLink",
                               &error,
                               NULL,
                               "i", arg_ifindex);
        if (r < 0) {
                if (arg_ifindex_permissive &&
                    sd_bus_error_has_name(&error, BUS_ERROR_NO_SUCH_LINK))
                        return 0;

                return log_error_errno(r, "Failed to revert interface configuration: %s", bus_error_message(&error, r));
        }

        return 0;
}

static void help_protocol_types(void) {
        if (arg_legend)
                puts("Known protocol types:");
        puts("dns\nllmnr\nllmnr-ipv4\nllmnr-ipv6\nmdns\nmdns-ipv4\nmdns-ipv6");
}

static void help_dns_types(void) {
        const char *t;
        int i;

        if (arg_legend)
                puts("Known DNS RR types:");
        for (i = 0; i < _DNS_TYPE_MAX; i++) {
                t = dns_type_to_string(i);
                if (t)
                        puts(t);
        }
}

static void help_dns_classes(void) {
        const char *t;
        int i;

        if (arg_legend)
                puts("Known DNS RR classes:");
        for (i = 0; i < _DNS_CLASS_MAX; i++) {
                t = dns_class_to_string(i);
                if (t)
                        puts(t);
        }
}

static void compat_help(void) {
        printf("%1$s [OPTIONS...] HOSTNAME|ADDRESS...\n"
               "%1$s [OPTIONS...] --service [[NAME] TYPE] DOMAIN\n"
               "%1$s [OPTIONS...] --openpgp EMAIL@DOMAIN...\n"
               "%1$s [OPTIONS...] --statistics\n"
               "%1$s [OPTIONS...] --reset-statistics\n"
               "\n"
               "Resolve domain names, IPv4 and IPv6 addresses, DNS records, and services.\n\n"
               "  -h --help                 Show this help\n"
               "     --version              Show package version\n"
               "     --no-pager             Do not pipe output into a pager\n"
               "  -4                        Resolve IPv4 addresses\n"
               "  -6                        Resolve IPv6 addresses\n"
               "  -i --interface=INTERFACE  Look on interface\n"
               "  -p --protocol=PROTO|help  Look via protocol\n"
               "  -t --type=TYPE|help       Query RR with DNS type\n"
               "  -c --class=CLASS|help     Query RR with DNS class\n"
               "     --service              Resolve service (SRV)\n"
               "     --service-address=BOOL Resolve address for services (default: yes)\n"
               "     --service-txt=BOOL     Resolve TXT records for services (default: yes)\n"
               "     --openpgp              Query OpenPGP public key\n"
               "     --tlsa                 Query TLS public key\n"
               "     --cname=BOOL           Follow CNAME redirects (default: yes)\n"
               "     --search=BOOL          Use search domains for single-label names\n"
               "                                                              (default: yes)\n"
               "     --raw[=payload|packet] Dump the answer as binary data\n"
               "     --legend=BOOL          Print headers and additional info (default: yes)\n"
               "     --statistics           Show resolver statistics\n"
               "     --reset-statistics     Reset resolver statistics\n"
               "     --status               Show link and server status\n"
               "     --flush-caches         Flush all local DNS caches\n"
               "     --reset-server-features\n"
               "                            Forget learnt DNS server feature levels\n"
               "     --set-dns=SERVER       Set per-interface DNS server address\n"
               "     --set-domain=DOMAIN    Set per-interface search domain\n"
               "     --set-llmnr=MODE       Set per-interface LLMNR mode\n"
               "     --set-mdns=MODE        Set per-interface MulticastDNS mode\n"
               "     --set-dnssec=MODE      Set per-interface DNSSEC mode\n"
               "     --set-nta=DOMAIN       Set per-interface DNSSEC NTA\n"
               "     --revert               Revert per-interface configuration\n"
               , program_invocation_short_name);
}

static void native_help(void) {
        printf("%1$s [OPTIONS...] {COMMAND} ...\n"
               "\n"
               "Send control commands to the network name resolution manager, or\n"
               "resolve domain names, IPv4 and IPv6 addresses, DNS records, and services.\n"
               "\n"
               "  -h --help                    Show this help\n"
               "     --version                 Show package version\n"
               "     --no-pager                Do not pipe output into a pager\n"
               "  -4                           Resolve IPv4 addresses\n"
               "  -6                           Resolve IPv6 addresses\n"
               "  -i --interface=INTERFACE     Look on interface\n"
               "  -p --protocol=PROTO|help     Look via protocol\n"
               "  -t --type=TYPE|help          Query RR with DNS type\n"
               "  -c --class=CLASS|help        Query RR with DNS class\n"
               "     --service-address=BOOL    Resolve address for services (default: yes)\n"
               "     --service-txt=BOOL        Resolve TXT records for services (default: yes)\n"
               "     --cname=BOOL              Follow CNAME redirects (default: yes)\n"
               "     --search=BOOL             Use search domains for single-label names\n"
               "                                                              (default: yes)\n"
               "     --raw[=payload|packet]    Dump the answer as binary data\n"
               "     --legend=BOOL             Print headers and additional info (default: yes)\n"
               "\n"
               "Commands:\n"
               "  query HOSTNAME|ADDRESS...    Resolve domain names, IPv4 and IPv6 addresses\n"
               "  service [[NAME] TYPE] DOMAIN Resolve service (SRV)\n"
               "  openpgp EMAIL@DOMAIN...      Query OpenPGP public key\n"
               "  tlsa DOMAIN[:PORT]...        Query TLS public key\n"
               "  status [LINK...]             Show link and server status\n"
               "  statistics                   Show resolver statistics\n"
               "  reset-statistics             Reset resolver statistics\n"
               "  flush-caches                 Flush all local DNS caches\n"
               "  reset-server-features        Forget learnt DNS server feature levels\n"
               "  dns [LINK [SERVER...]]       Get/set per-interface DNS server address\n"
               "  domain [LINK [DOMAIN...]]    Get/set per-interface search domain\n"
               "  llmnr [LINK [MODE]]          Get/set per-interface LLMNR mode\n"
               "  mdns [LINK [MODE]]           Get/set per-interface MulticastDNS mode\n"
               "  dnssec [LINK [MODE]]         Get/set per-interface DNSSEC mode\n"
               "  nta [LINK [DOMAIN...]]       Get/set per-interface DNSSEC NTA\n"
               "  revert LINK                  Revert per-interface configuration\n"
               , program_invocation_short_name);
}

static int verb_help(int argc, char **argv, void *userdata) {
        native_help();
        return 0;
}

static int compat_parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_LEGEND,
                ARG_SERVICE,
                ARG_CNAME,
                ARG_SERVICE_ADDRESS,
                ARG_SERVICE_TXT,
                ARG_OPENPGP,
                ARG_TLSA,
                ARG_RAW,
                ARG_SEARCH,
                ARG_STATISTICS,
                ARG_RESET_STATISTICS,
                ARG_STATUS,
                ARG_FLUSH_CACHES,
                ARG_RESET_SERVER_FEATURES,
                ARG_NO_PAGER,
                ARG_SET_DNS,
                ARG_SET_DOMAIN,
                ARG_SET_LLMNR,
                ARG_SET_MDNS,
                ARG_SET_DNSSEC,
                ARG_SET_NTA,
                ARG_REVERT_LINK,
        };

        static const struct option options[] = {
                { "help",                  no_argument,       NULL, 'h'                       },
                { "version",               no_argument,       NULL, ARG_VERSION               },
                { "type",                  required_argument, NULL, 't'                       },
                { "class",                 required_argument, NULL, 'c'                       },
                { "legend",                required_argument, NULL, ARG_LEGEND                },
                { "interface",             required_argument, NULL, 'i'                       },
                { "protocol",              required_argument, NULL, 'p'                       },
                { "cname",                 required_argument, NULL, ARG_CNAME                 },
                { "service",               no_argument,       NULL, ARG_SERVICE               },
                { "service-address",       required_argument, NULL, ARG_SERVICE_ADDRESS       },
                { "service-txt",           required_argument, NULL, ARG_SERVICE_TXT           },
                { "openpgp",               no_argument,       NULL, ARG_OPENPGP               },
                { "tlsa",                  optional_argument, NULL, ARG_TLSA                  },
                { "raw",                   optional_argument, NULL, ARG_RAW                   },
                { "search",                required_argument, NULL, ARG_SEARCH                },
                { "statistics",            no_argument,       NULL, ARG_STATISTICS,           },
                { "reset-statistics",      no_argument,       NULL, ARG_RESET_STATISTICS      },
                { "status",                no_argument,       NULL, ARG_STATUS                },
                { "flush-caches",          no_argument,       NULL, ARG_FLUSH_CACHES          },
                { "reset-server-features", no_argument,       NULL, ARG_RESET_SERVER_FEATURES },
                { "no-pager",              no_argument,       NULL, ARG_NO_PAGER              },
                { "set-dns",               required_argument, NULL, ARG_SET_DNS               },
                { "set-domain",            required_argument, NULL, ARG_SET_DOMAIN            },
                { "set-llmnr",             required_argument, NULL, ARG_SET_LLMNR             },
                { "set-mdns",              required_argument, NULL, ARG_SET_MDNS              },
                { "set-dnssec",            required_argument, NULL, ARG_SET_DNSSEC            },
                { "set-nta",               required_argument, NULL, ARG_SET_NTA               },
                { "revert",                no_argument,       NULL, ARG_REVERT_LINK           },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h46i:t:c:p:", options, NULL)) >= 0)
                switch(c) {

                case 'h':
                        compat_help();
                        return 0; /* done */;

                case ARG_VERSION:
                        return version();

                case '4':
                        arg_family = AF_INET;
                        break;

                case '6':
                        arg_family = AF_INET6;
                        break;

                case 'i':
                        r = parse_ifindex_with_warn(optarg);
                        if (r < 0)
                                return r;

                        arg_ifindex = r;
                        break;

                case 't':
                        if (streq(optarg, "help")) {
                                help_dns_types();
                                return 0;
                        }

                        r = dns_type_from_string(optarg);
                        if (r < 0) {
                                log_error("Failed to parse RR record type %s", optarg);
                                return r;
                        }
                        arg_type = (uint16_t) r;
                        assert((int) arg_type == r);

                        arg_mode = MODE_RESOLVE_RECORD;
                        break;

                case 'c':
                        if (streq(optarg, "help")) {
                                help_dns_classes();
                                return 0;
                        }

                        r = dns_class_from_string(optarg);
                        if (r < 0) {
                                log_error("Failed to parse RR record class %s", optarg);
                                return r;
                        }
                        arg_class = (uint16_t) r;
                        assert((int) arg_class == r);

                        break;

                case ARG_LEGEND:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --legend= argument");

                        arg_legend = r;
                        break;

                case 'p':
                        if (streq(optarg, "help")) {
                                help_protocol_types();
                                return 0;
                        } else if (streq(optarg, "dns"))
                                arg_flags |= SD_RESOLVED_DNS;
                        else if (streq(optarg, "llmnr"))
                                arg_flags |= SD_RESOLVED_LLMNR;
                        else if (streq(optarg, "llmnr-ipv4"))
                                arg_flags |= SD_RESOLVED_LLMNR_IPV4;
                        else if (streq(optarg, "llmnr-ipv6"))
                                arg_flags |= SD_RESOLVED_LLMNR_IPV6;
                        else if (streq(optarg, "mdns"))
                                arg_flags |= SD_RESOLVED_MDNS;
                        else if (streq(optarg, "mdns-ipv4"))
                                arg_flags |= SD_RESOLVED_MDNS_IPV4;
                        else if (streq(optarg, "mdns-ipv6"))
                                arg_flags |= SD_RESOLVED_MDNS_IPV6;
                        else {
                                log_error("Unknown protocol specifier: %s", optarg);
                                return -EINVAL;
                        }

                        break;

                case ARG_SERVICE:
                        arg_mode = MODE_RESOLVE_SERVICE;
                        break;

                case ARG_OPENPGP:
                        arg_mode = MODE_RESOLVE_OPENPGP;
                        break;

                case ARG_TLSA:
                        arg_mode = MODE_RESOLVE_TLSA;
                        if (optarg) {
                                arg_service_family = service_family_from_string(optarg);
                                if (arg_service_family < 0) {
                                        log_error("Unknown service family \"%s\".", optarg);
                                        return -EINVAL;
                                }
                        }
                        break;

                case ARG_RAW:
                        if (on_tty()) {
                                log_error("Refusing to write binary data to tty.");
                                return -ENOTTY;
                        }

                        if (optarg == NULL || streq(optarg, "payload"))
                                arg_raw = RAW_PAYLOAD;
                        else if (streq(optarg, "packet"))
                                arg_raw = RAW_PACKET;
                        else {
                                log_error("Unknown --raw specifier \"%s\".", optarg);
                                return -EINVAL;
                        }

                        arg_legend = false;
                        break;

                case ARG_CNAME:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --cname= argument.");
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_CNAME, r == 0);
                        break;

                case ARG_SERVICE_ADDRESS:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --service-address= argument.");
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_ADDRESS, r == 0);
                        break;

                case ARG_SERVICE_TXT:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --service-txt= argument.");
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_TXT, r == 0);
                        break;

                case ARG_SEARCH:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --search argument.");
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_SEARCH, r == 0);
                        break;

                case ARG_STATISTICS:
                        arg_mode = MODE_STATISTICS;
                        break;

                case ARG_RESET_STATISTICS:
                        arg_mode = MODE_RESET_STATISTICS;
                        break;

                case ARG_FLUSH_CACHES:
                        arg_mode = MODE_FLUSH_CACHES;
                        break;

                case ARG_RESET_SERVER_FEATURES:
                        arg_mode = MODE_RESET_SERVER_FEATURES;
                        break;

                case ARG_STATUS:
                        arg_mode = MODE_STATUS;
                        break;

                case ARG_NO_PAGER:
                        arg_no_pager = true;
                        break;

                case ARG_SET_DNS:
                        r = strv_extend(&arg_set_dns, optarg);
                        if (r < 0)
                                return log_oom();

                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_DOMAIN:
                        r = strv_extend(&arg_set_domain, optarg);
                        if (r < 0)
                                return log_oom();

                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_LLMNR:
                        arg_set_llmnr = optarg;
                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_MDNS:
                        arg_set_mdns = optarg;
                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_DNSSEC:
                        arg_set_dnssec = optarg;
                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_SET_NTA:
                        r = strv_extend(&arg_set_nta, optarg);
                        if (r < 0)
                                return log_oom();

                        arg_mode = MODE_SET_LINK;
                        break;

                case ARG_REVERT_LINK:
                        arg_mode = MODE_REVERT_LINK;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (arg_type == 0 && arg_class != 0) {
                log_error("--class= may only be used in conjunction with --type=.");
                return -EINVAL;
        }

        if (arg_type != 0 && arg_mode == MODE_RESOLVE_SERVICE) {
                log_error("--service and --type= may not be combined.");
                return -EINVAL;
        }

        if (arg_type != 0 && arg_class == 0)
                arg_class = DNS_CLASS_IN;

        if (arg_class != 0 && arg_type == 0)
                arg_type = DNS_TYPE_A;

        if (IN_SET(arg_mode, MODE_SET_LINK, MODE_REVERT_LINK)) {

                if (arg_ifindex <= 0) {
                        log_error("--set-dns=, --set-domain=, --set-llmnr=, --set-mdns=, --set-dnssec=, --set-nta= and --revert require --interface=.");
                        return -EINVAL;
                }

                if (arg_ifindex == LOOPBACK_IFINDEX) {
                        log_error("Interface can't be the loopback interface (lo). Sorry.");
                        return -EINVAL;
                }
        }

        return 1 /* work to do */;
}

static int native_parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_LEGEND,
                ARG_CNAME,
                ARG_SERVICE_ADDRESS,
                ARG_SERVICE_TXT,
                ARG_SERVICE_FAMILY,
                ARG_RAW,
                ARG_SEARCH,
                ARG_NO_PAGER,
        };

        static const struct option options[] = {
                { "help",                  no_argument,       NULL, 'h'                       },
                { "version",               no_argument,       NULL, ARG_VERSION               },
                { "type",                  required_argument, NULL, 't'                       },
                { "class",                 required_argument, NULL, 'c'                       },
                { "legend",                required_argument, NULL, ARG_LEGEND                },
                { "interface",             required_argument, NULL, 'i'                       },
                { "protocol",              required_argument, NULL, 'p'                       },
                { "cname",                 required_argument, NULL, ARG_CNAME                 },
                { "service-address",       required_argument, NULL, ARG_SERVICE_ADDRESS       },
                { "service-txt",           required_argument, NULL, ARG_SERVICE_TXT           },
                { "service-family",        required_argument, NULL, ARG_SERVICE_FAMILY        },
                { "raw",                   optional_argument, NULL, ARG_RAW                   },
                { "search",                required_argument, NULL, ARG_SEARCH                },
                { "no-pager",              no_argument,       NULL, ARG_NO_PAGER              },
                {}
        };

        int c, r;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h46i:t:c:p:", options, NULL)) >= 0)
                switch(c) {

                case 'h':
                        native_help();
                        return 0; /* done */;

                case ARG_VERSION:
                        return version();

                case '4':
                        arg_family = AF_INET;
                        break;

                case '6':
                        arg_family = AF_INET6;
                        break;

                case 'i':
                        r = parse_ifindex_with_warn(optarg);
                        if (r < 0)
                                return r;

                        arg_ifindex = r;
                        break;

                case 't':
                        if (streq(optarg, "help")) {
                                help_dns_types();
                                return 0;
                        }

                        r = dns_type_from_string(optarg);
                        if (r < 0) {
                                log_error("Failed to parse RR record type %s", optarg);
                                return r;
                        }
                        arg_type = (uint16_t) r;
                        assert((int) arg_type == r);

                        break;

                case 'c':
                        if (streq(optarg, "help")) {
                                help_dns_classes();
                                return 0;
                        }

                        r = dns_class_from_string(optarg);
                        if (r < 0) {
                                log_error("Failed to parse RR record class %s", optarg);
                                return r;
                        }
                        arg_class = (uint16_t) r;
                        assert((int) arg_class == r);

                        break;

                case ARG_LEGEND:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --legend= argument");

                        arg_legend = r;
                        break;

                case 'p':
                        if (streq(optarg, "help")) {
                                help_protocol_types();
                                return 0;
                        } else if (streq(optarg, "dns"))
                                arg_flags |= SD_RESOLVED_DNS;
                        else if (streq(optarg, "llmnr"))
                                arg_flags |= SD_RESOLVED_LLMNR;
                        else if (streq(optarg, "llmnr-ipv4"))
                                arg_flags |= SD_RESOLVED_LLMNR_IPV4;
                        else if (streq(optarg, "llmnr-ipv6"))
                                arg_flags |= SD_RESOLVED_LLMNR_IPV6;
                        else if (streq(optarg, "mdns"))
                                arg_flags |= SD_RESOLVED_MDNS;
                        else if (streq(optarg, "mdns-ipv4"))
                                arg_flags |= SD_RESOLVED_MDNS_IPV4;
                        else if (streq(optarg, "mdns-ipv6"))
                                arg_flags |= SD_RESOLVED_MDNS_IPV6;
                        else {
                                log_error("Unknown protocol specifier: %s", optarg);
                                return -EINVAL;
                        }

                        break;

                case ARG_RAW:
                        if (on_tty()) {
                                log_error("Refusing to write binary data to tty.");
                                return -ENOTTY;
                        }

                        if (optarg == NULL || streq(optarg, "payload"))
                                arg_raw = RAW_PAYLOAD;
                        else if (streq(optarg, "packet"))
                                arg_raw = RAW_PACKET;
                        else {
                                log_error("Unknown --raw specifier \"%s\".", optarg);
                                return -EINVAL;
                        }

                        arg_legend = false;
                        break;

                case ARG_CNAME:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --cname= argument.");
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_CNAME, r == 0);
                        break;

                case ARG_SERVICE_ADDRESS:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --service-address= argument.");
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_ADDRESS, r == 0);
                        break;

                case ARG_SERVICE_TXT:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --service-txt= argument.");
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_TXT, r == 0);
                        break;

                case ARG_SERVICE_FAMILY:
                        arg_service_family = service_family_from_string(optarg);
                        if (arg_service_family < 0) {
                                log_error("Unknown service family \"%s\".", optarg);
                                return -EINVAL;
                        }
                        break;

                case ARG_SEARCH:
                        r = parse_boolean(optarg);
                        if (r < 0)
                                return log_error_errno(r, "Failed to parse --search argument.");
                        SET_FLAG(arg_flags, SD_RESOLVED_NO_SEARCH, r == 0);
                        break;

                case ARG_NO_PAGER:
                        arg_no_pager = true;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (arg_type == 0 && arg_class != 0) {
                log_error("--class= may only be used in conjunction with --type=.");
                return -EINVAL;
        }

        if (arg_type != 0 && arg_class == 0)
                arg_class = DNS_CLASS_IN;

        if (arg_class != 0 && arg_type == 0)
                arg_type = DNS_TYPE_A;

        return 1 /* work to do */;
}

static int native_main(int argc, char *argv[], sd_bus *bus) {

        static const Verb verbs[] = {
                { "help",                  VERB_ANY, VERB_ANY, 0,            verb_help             },
                { "status",                VERB_ANY, VERB_ANY, VERB_DEFAULT, verb_status           },
                { "query",                 2,        VERB_ANY, 0,            verb_query            },
                { "service",               2,        4,        0,            verb_service          },
                { "openpgp",               2,        VERB_ANY, 0,            verb_openpgp          },
                { "tlsa",                  2,        VERB_ANY, 0,            verb_tlsa             },
                { "statistics",            VERB_ANY, 1,        0,            show_statistics       },
                { "reset-statistics",      VERB_ANY, 1,        0,            reset_statistics      },
                { "flush-caches",          VERB_ANY, 1,        0,            flush_caches          },
                { "reset-server-features", VERB_ANY, 1,        0,            reset_server_features },
                { "dns",                   VERB_ANY, VERB_ANY, 0,            verb_dns              },
                { "domain",                VERB_ANY, VERB_ANY, 0,            verb_domain           },
                { "llmnr",                 VERB_ANY, 2,        0,            verb_llmnr            },
                { "mdns",                  VERB_ANY, 2,        0,            verb_mdns             },
                { "dnssec",                VERB_ANY, 2,        0,            verb_dnssec           },
                { "nta",                   VERB_ANY, VERB_ANY, 0,            verb_nta              },
                { "revert",                VERB_ANY, 1,        0,            verb_revert_link      },
                {}
        };

        return dispatch_verb(argc, argv, verbs, bus);
}

static int translate(const char *verb, size_t num_args, char **args, sd_bus *bus) {
        char **fake, **p;
        size_t num, i;

        assert(verb);
        assert(num_args == 0 || args);

        num = num_args + 1;

        p = fake = newa0(char *, num + 1);
        *p++ = (char *) verb;
        for (i = 0; i < num_args; i++)
                *p++ = args[i];

        optind = 0;
        return native_main((int) num, fake, bus);
}

static int compat_main(int argc, char *argv[], sd_bus *bus) {
        int r = 0;

        switch (arg_mode) {
        case MODE_RESOLVE_HOST:
        case MODE_RESOLVE_RECORD:
                return translate("query", argc - optind, argv + optind, bus);

        case MODE_RESOLVE_SERVICE:
                return translate("service", argc - optind, argv + optind, bus);

        case MODE_RESOLVE_OPENPGP:
                return translate("openpgp", argc - optind, argv + optind, bus);

        case MODE_RESOLVE_TLSA:
                return translate("tlsa", argc - optind, argv + optind, bus);

        case MODE_STATISTICS:
                return translate("statistics", 0, NULL, bus);

        case MODE_RESET_STATISTICS:
                return translate("reset-statistics", 0, NULL, bus);

        case MODE_FLUSH_CACHES:
                return translate("flush-caches", 0, NULL, bus);

        case MODE_RESET_SERVER_FEATURES:
                return translate("reset-server-features", 0, NULL, bus);

        case MODE_STATUS:
                return translate("status", argc - optind, argv + optind, bus);

        case MODE_SET_LINK:
                if (arg_set_dns) {
                        r = translate("dns", strv_length(arg_set_dns), arg_set_dns, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_domain) {
                        r = translate("domain", strv_length(arg_set_domain), arg_set_domain, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_nta) {
                        r = translate("nta", strv_length(arg_set_nta), arg_set_nta, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_llmnr) {
                        r = translate("llmnr", 1, (char **) &arg_set_llmnr, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_mdns) {
                        r = translate("mdns", 1, (char **) &arg_set_mdns, bus);
                        if (r < 0)
                                return r;
                }

                if (arg_set_dnssec) {
                        r = translate("dnssec", 1, (char **) &arg_set_dnssec, bus);
                        if (r < 0)
                                return r;
                }

                return r;

        case MODE_REVERT_LINK:
                return translate("revert", 0, NULL, bus);

        case _MODE_INVALID:
                assert_not_reached("invalid mode");
        }

        return 0;
}

int main(int argc, char **argv) {
        sd_bus *bus = NULL;
        int r;

        setlocale(LC_ALL, "");
        log_parse_environment();
        log_open();

        if (streq(program_invocation_short_name, "resolvconf"))
                r = resolvconf_parse_argv(argc, argv);
        else if (streq(program_invocation_short_name, "systemd-resolve"))
                r = compat_parse_argv(argc, argv);
        else
                r = native_parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        r = sd_bus_open_system(&bus);
        if (r < 0) {
                log_error_errno(r, "sd_bus_open_system: %m");
                goto finish;
        }

        if (streq(program_invocation_short_name, "systemd-resolve"))
                r = compat_main(argc, argv, bus);
        else
                r = native_main(argc, argv, bus);

finish:
        /* make sure we terminate the bus connection first, and then close the
         * pager, see issue #3543 for the details. */
        sd_bus_flush_close_unref(bus);
        pager_close();

        strv_free(arg_set_dns);
        strv_free(arg_set_domain);
        strv_free(arg_set_nta);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
