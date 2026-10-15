/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Failure of protocol defects (§10.4). Detection and clearance details belong to G.8021; the
 * behaviour here is the overview of §10.4 with a configurable period (default 3.5 x 5 s).
 *
 *  FOP-PM (provisioning mismatch): the RPL owner receives R-APS(NR,RB) carrying a node ID other
 *         than its own — a second owner is configured. Cleared when no such message has been
 *         received for fop_timeout_ms.
 *  FOP-TO (timeout): no R-APS received on a ring port for fop_timeout_ms. Not reported while
 *         the port has SF, nor for the RPL port of the owner / neighbour in Idle (both RPL ends
 *         blocked, nothing crosses the RPL). Cleared by the next R-APS on that port. Off by
 *         default: with an RPL neighbour configured, Idle rings legitimately stay silent on some
 *         ports, which G.8021 handles with information we do not have.
 */
#include "erps_internal.h"

#include <string.h>

static void raise_event(erps_t *e, erps_event_type_t type, bool raised, erps_port_t p,
                        const uint8_t *node)
{
    erps_event_t ev = { .type = type };
    ev.u.fop.raised = raised;
    ev.u.fop.port = p;
    if (node)
        memcpy(ev.u.fop.node_id, node, 6);
    erps__notify(e, &ev);
}

void erps__fop_reset(erps_t *e)
{
    e->fop_pm = false;
    e->fop_pm_deadline = ERPS_TIME_NEVER;
    for (int p = 0; p < 2; p++) {
        e->port[p].fop_to = false;
        e->port[p].fop_to_deadline = (e->running && e->cfg.fop_to_enable)
                                         ? erps__deadline_after(e, e->cfg.fop_timeout_ms)
                                         : ERPS_TIME_NEVER;
    }
}

void erps__fop_rx(erps_t *e, erps_port_t p, const erps_raps_msg_t *m)
{
    struct erps_port *pp = &e->port[p];
    if (e->cfg.fop_to_enable) {
        if (pp->fop_to) {
            pp->fop_to = false;
            erps__log(e, ERPS_LOG_NOTICE, "FOP-TO cleared on port %d", (int)p);
            raise_event(e, ERPS_EV_FOP_TO, false, p, NULL);
        }
        pp->fop_to_deadline = erps__deadline_after(e, e->cfg.fop_timeout_ms);
    }

    if (e->cfg.fop_pm_enable && erps__is_owner(e) && m->req == ERPS_RAPS_NR && m->rb &&
        memcmp(m->node_id, e->cfg.node_id, 6) != 0) {
        if (!e->fop_pm) {
            char b[24];
            erps__fmt_mac(m->node_id, b, sizeof b);
            e->fop_pm = true;
            e->ctr.fop_pm++;
            erps__log(e, ERPS_LOG_WARN, "FOP-PM: R-APS(NR,RB) from %s on port %d: another RPL owner?",
                      b, (int)p);
            raise_event(e, ERPS_EV_FOP_PM, true, p, m->node_id);
        }
        e->fop_pm_deadline = erps__deadline_after(e, e->cfg.fop_timeout_ms);
    }
}

static bool fop_to_suppressed(const erps_t *e, erps_port_t p)
{
    if (e->port[p].sf)
        return true;
    if (e->cfg.role != ERPS_ROLE_NONE && p == e->cfg.rpl_port && e->state == ERPS_STATE_IDLE)
        return true;
    return false;
}

void erps__fop_poll(erps_t *e)
{
    if (e->fop_pm && e->now >= e->fop_pm_deadline) {
        e->fop_pm = false;
        e->fop_pm_deadline = ERPS_TIME_NEVER;
        erps__log(e, ERPS_LOG_NOTICE, "FOP-PM cleared");
        raise_event(e, ERPS_EV_FOP_PM, false, ERPS_PORT_NONE, NULL);
    }
    if (!e->cfg.fop_to_enable)
        return;
    for (int i = 0; i < 2; i++) {
        erps_port_t p = (erps_port_t)i;
        struct erps_port *pp = &e->port[p];
        if (pp->fop_to_deadline == ERPS_TIME_NEVER || e->now < pp->fop_to_deadline)
            continue;
        if (fop_to_suppressed(e, p)) {
            pp->fop_to_deadline = erps__deadline_after(e, e->cfg.fop_timeout_ms);
            continue;
        }
        pp->fop_to_deadline = ERPS_TIME_NEVER;
        if (!pp->fop_to) {
            pp->fop_to = true;
            e->ctr.fop_to++;
            erps__log(e, ERPS_LOG_WARN, "FOP-TO: no R-APS on port %d for %u ms", (int)p,
                      e->cfg.fop_timeout_ms);
            raise_event(e, ERPS_EV_FOP_TO, true, p, NULL);
        }
    }
}

erps_time_t erps__fop_next(const erps_t *e)
{
    erps_time_t t = e->fop_pm ? e->fop_pm_deadline : ERPS_TIME_NEVER;
    for (int p = 0; p < 2; p++)
        if (e->port[p].fop_to_deadline < t)
            t = e->port[p].fop_to_deadline;
    return t;
}
