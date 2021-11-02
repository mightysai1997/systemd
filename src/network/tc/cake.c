/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright © 2020 VMware, Inc. */

#include <linux/pkt_sched.h>

#include "alloc-util.h"
#include "cake.h"
#include "conf-parser.h"
#include "netlink-util.h"
#include "parse-util.h"
#include "qdisc.h"
#include "string-table.h"
#include "string-util.h"

static int cake_init(QDisc *qdisc) {
        CommonApplicationsKeptEnhanced *c;

        assert(qdisc);

        c = CAKE(qdisc);

        c->autorate = -1;
        c->compensation_mode = _CAKE_COMPENSATION_MODE_INVALID;

        return 0;
}

static int cake_fill_message(Link *link, QDisc *qdisc, sd_netlink_message *req) {
        CommonApplicationsKeptEnhanced *c;
        int r;

        assert(link);
        assert(qdisc);
        assert(req);

        c = CAKE(qdisc);

        r = sd_netlink_message_open_container_union(req, TCA_OPTIONS, "cake");
        if (r < 0)
                return log_link_error_errno(link, r, "Could not open container TCA_OPTIONS: %m");

        if (c->bandwidth > 0) {
                r = sd_netlink_message_append_u64(req, TCA_CAKE_BASE_RATE64, c->bandwidth);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append TCA_CAKE_BASE_RATE64 attribute: %m");
        }

        if (c->autorate >= 0) {
                r = sd_netlink_message_append_u32(req, TCA_CAKE_AUTORATE, c->autorate);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append TCA_CAKE_AUTORATE attribute: %m");
        }

        if (c->overhead_set) {
                r = sd_netlink_message_append_s32(req, TCA_CAKE_OVERHEAD, c->overhead);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append TCA_CAKE_OVERHEAD attribute: %m");
        }

        if (c->compensation_mode >= 0) {
                r = sd_netlink_message_append_u32(req, TCA_CAKE_ATM, c->compensation_mode);
                if (r < 0)
                        return log_link_error_errno(link, r, "Could not append TCA_CAKE_ATM attribute: %m");
        }

        r = sd_netlink_message_close_container(req);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not close container TCA_OPTIONS: %m");

        return 0;
}

int config_parse_cake_bandwidth(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(qdisc_free_or_set_invalidp) QDisc *qdisc = NULL;
        CommonApplicationsKeptEnhanced *c;
        Network *network = data;
        uint64_t k;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = qdisc_new_static(QDISC_KIND_CAKE, network, filename, section_line, &qdisc);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "More than one kind of queueing discipline, ignoring assignment: %m");
                return 0;
        }

        c = CAKE(qdisc);

        if (isempty(rvalue)) {
                c->bandwidth = 0;

                TAKE_PTR(qdisc);
                return 0;
        }

        r = parse_size(rvalue, 1000, &k);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse '%s=', ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        c->bandwidth = k/8;
        TAKE_PTR(qdisc);

        return 0;
}

int config_parse_cake_overhead(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(qdisc_free_or_set_invalidp) QDisc *qdisc = NULL;
        CommonApplicationsKeptEnhanced *c;
        Network *network = data;
        int32_t v;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = qdisc_new_static(QDISC_KIND_CAKE, network, filename, section_line, &qdisc);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "More than one kind of queueing discipline, ignoring assignment: %m");
                return 0;
        }

        c = CAKE(qdisc);

        if (isempty(rvalue)) {
                c->overhead_set = false;
                TAKE_PTR(qdisc);
                return 0;
        }

        r = safe_atoi32(rvalue, &v);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse '%s=', ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }
        if (v < -64 || v > 256) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Invalid '%s=', ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        c->overhead = v;
        c->overhead_set = true;
        TAKE_PTR(qdisc);
        return 0;
}

int config_parse_cake_tristate(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(qdisc_free_or_set_invalidp) QDisc *qdisc = NULL;
        CommonApplicationsKeptEnhanced *c;
        Network *network = data;
        int *dest, r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = qdisc_new_static(QDISC_KIND_CAKE, network, filename, section_line, &qdisc);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "More than one kind of queueing discipline, ignoring assignment: %m");
                return 0;
        }

        c = CAKE(qdisc);

        if (streq(lvalue, "AutoRateIngress"))
                dest = &c->autorate;
        else
                assert_not_reached();

        if (isempty(rvalue)) {
                *dest = -1;
                TAKE_PTR(qdisc);
                return 0;
        }

        r = parse_boolean(rvalue);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse '%s=', ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        *dest = r;
        TAKE_PTR(qdisc);
        return 0;
}

static const char * const cake_compensation_mode_table[_CAKE_COMPENSATION_MODE_MAX] = {
        [CAKE_COMPENSATION_MODE_NONE] = "none",
        [CAKE_COMPENSATION_MODE_ATM]  = "atm",
        [CAKE_COMPENSATION_MODE_PTM]  = "ptm",
};

DEFINE_PRIVATE_STRING_TABLE_LOOKUP_FROM_STRING(cake_compensation_mode, CakeCompensationMode);

int config_parse_cake_compensation_mode(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        _cleanup_(qdisc_free_or_set_invalidp) QDisc *qdisc = NULL;
        CommonApplicationsKeptEnhanced *c;
        Network *network = data;
        CakeCompensationMode mode;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        r = qdisc_new_static(QDISC_KIND_CAKE, network, filename, section_line, &qdisc);
        if (r == -ENOMEM)
                return log_oom();
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "More than one kind of queueing discipline, ignoring assignment: %m");
                return 0;
        }

        c = CAKE(qdisc);

        if (isempty(rvalue)) {
                c->compensation_mode = _CAKE_COMPENSATION_MODE_INVALID;
                TAKE_PTR(qdisc);
                return 0;
        }

        mode = cake_compensation_mode_from_string(rvalue);
        if (mode < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, mode,
                           "Failed to parse '%s=', ignoring assignment: %s",
                           lvalue, rvalue);
                return 0;
        }

        c->compensation_mode = mode;
        TAKE_PTR(qdisc);
        return 0;
}

const QDiscVTable cake_vtable = {
        .object_size = sizeof(CommonApplicationsKeptEnhanced),
        .tca_kind = "cake",
        .init = cake_init,
        .fill_message = cake_fill_message,
};
