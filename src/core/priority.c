/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Priority logic (§10.1.1), local priority logic (§10.1.9) and the command filtering part of
 * the backward compatibility logic (§10.1.13).
 *
 * Inputs are split in two kinds:
 *
 *   persistent  conditions that hold until cleared and are re-read at every evaluation:
 *               latched local FS / MS, declared local SF (ignored in state D), WTR Running,
 *               WTB Running;
 *   events      one-shot inputs: Clear, a new FS / MS command, local clear SF, WTR / WTB
 *               Expires, and every R-APS message that passed the validity check and the guard
 *               timer (R-APS information is never stored, §10.1.1).
 *
 * On each event the top-priority request is max(event, persistent top) by Table 10-1. The
 * state machine (Table 10-2) is invoked with it, except when an R-APS message loses against a
 * persistent local request that has already been signalled: the top-priority request has then
 * not changed and re-running the row would only repeat (or, for rows 5/19/33/61, wrongly alter)
 * actions already taken. After any run that changes the node state, persistent requests are
 * re-evaluated, because they may depend on the state (local SF is ignored in state D; latched
 * commands are dropped when the state they created is left).
 */
#include "erps_internal.h"

#include <string.h>

void erps__prio_persistent(const erps_t *e, erps_req_t *out)
{
    memset(out, 0, sizeof *out);
    out->type = ERPS_REQ_NONE;
    out->port = ERPS_PORT_NONE;

    if (e->fs_active) {
        out->type = ERPS_REQ_FS;
        out->port = e->cmd_port;
        return;
    }
    uint8_t sf = erps__sf_mask(e);
    if (sf && e->state != ERPS_STATE_FORCED_SWITCH) { /* Table 10-1 note a) */
        out->type = ERPS_REQ_LOCAL_SF;
        out->ports = sf;
        /* the most recently signalled failed port stays the subject while it is still failed */
        if (e->top.type == ERPS_REQ_LOCAL_SF && e->top.port != ERPS_PORT_NONE &&
            (sf & (1u << e->top.port)))
            out->port = e->top.port;
        else
            out->port = (sf & 1u) ? ERPS_PORT0 : ERPS_PORT1;
        return;
    }
    if (e->ms_active) {
        out->type = ERPS_REQ_MS;
        out->port = e->cmd_port;
        return;
    }
    if (e->wtr_deadline != ERPS_TIME_NEVER) {
        out->type = ERPS_REQ_WTR_RUNNING;
        return;
    }
    if (e->wtb_deadline != ERPS_TIME_NEVER) {
        out->type = ERPS_REQ_WTB_RUNNING;
        return;
    }
}

bool erps__req_same(const erps_req_t *a, const erps_req_t *b)
{
    if (a->type != b->type)
        return false;
    switch (a->type) {
    case ERPS_REQ_FS:
    case ERPS_REQ_MS:
        return a->port == b->port;
    case ERPS_REQ_LOCAL_SF:
        return a->ports == b->ports;
    case ERPS_REQ_WTR_RUNNING:
    case ERPS_REQ_WTB_RUNNING:
    case ERPS_REQ_NONE:
        return true;
    default:
        return false; /* events are never "the same" request */
    }
}

/* Re-evaluate persistent requests after state changes (bounded; each step changes state or
 * signals a request that then equals the top). */
static void settle(erps_t *e)
{
    for (int i = 0; i < 8 && e->state_changed; i++) {
        e->state_changed = false;
        erps_req_t p;
        erps__prio_persistent(e, &p);
        if (p.type == ERPS_REQ_NONE || erps__req_same(&p, &e->top))
            return;
        erps__fsm_run(e, &p);
    }
    e->state_changed = false;
}

static void evaluate(erps_t *e, const erps_req_t *ev, bool is_raps)
{
    erps_req_t p;
    erps__prio_persistent(e, &p);
    e->state_changed = false;

    if (ev && ev->type > p.type) {
        erps__fsm_run(e, ev);
    } else if (p.type != ERPS_REQ_NONE && !erps__req_same(&p, &e->top)) {
        erps__fsm_run(e, &p);
    } else if (ev) {
        if (is_raps)
            e->ctr.rx_suppressed++;
        char b[96];
        erps_req_format(ev, b, sizeof b);
        erps__log(e, ERPS_LOG_TRACE, "%s outranked by %s: no state machine run", b,
                  erps_req_type_str(p.type));
    }
    settle(e);
}

void erps__prio_event(erps_t *e, const erps_req_t *ev)
{
    evaluate(e, ev, false);
}

void erps__prio_raps(erps_t *e, const erps_req_t *ev)
{
    evaluate(e, ev, true);
}

static void reject(erps_t *e, erps_cmd_t cmd, erps_port_t port, erps_reject_t why,
                   erps_reject_t *out)
{
    if (out)
        *out = why;
    e->ctr.commands_rejected++;
    erps__log(e, ERPS_LOG_NOTICE, "command %s%s%s rejected: %s", erps_cmd_str(cmd),
              cmd == ERPS_CMD_CLEAR ? "" : " port ",
              cmd == ERPS_CMD_CLEAR ? "" : (port == ERPS_PORT0 ? "0" : "1"), erps_reject_str(why));
    erps_event_t ev = { .type = ERPS_EV_CMD_REJECTED };
    ev.u.reject.cmd = cmd;
    ev.u.reject.port = port;
    ev.u.reject.reason = why;
    erps__notify(e, &ev);
}

erps_err_t erps__prio_command(erps_t *e, erps_cmd_t cmd, erps_port_t port, erps_reject_t *why)
{
    if (why)
        *why = ERPS_REJECT_NONE;
    e->ctr.commands++;

    if (!e->running) {
        reject(e, cmd, port, ERPS_REJECT_NOT_RUNNING, why);
        return ERPS_E_REJECTED;
    }

    erps_req_t req;
    memset(&req, 0, sizeof req);
    req.port = port;

    erps_req_t p;
    erps__prio_persistent(e, &p);

    switch (cmd) {
    case ERPS_CMD_FORCED_SWITCH:
        if (e->cfg.compat_version == 1) { /* §10.1.13 a1 */
            reject(e, cmd, port, ERPS_REJECT_COMPAT_V1, why);
            return ERPS_E_REJECTED;
        }
        if (e->fs_active) { /* §10.2.4: accepted except at a node with a prior local FS */
            reject(e, cmd, port, ERPS_REJECT_FS_ACTIVE, why);
            return ERPS_E_REJECTED;
        }
        req.type = ERPS_REQ_FS;
        erps__log(e, ERPS_LOG_NOTICE, "forced switch on port %d", (int)port);
        evaluate(e, &req, false);
        if (e->state == ERPS_STATE_FORCED_SWITCH) {
            e->fs_active = true;
            e->ms_active = false;
            e->cmd_port = port;
        }
        return ERPS_OK;

    case ERPS_CMD_MANUAL_SWITCH:
        if (e->cfg.compat_version == 1) {
            reject(e, cmd, port, ERPS_REJECT_COMPAT_V1, why);
            return ERPS_E_REJECTED;
        }
        if (p.type > ERPS_REQ_MS) { /* local SF or FS outranks it */
            reject(e, cmd, port, ERPS_REJECT_HIGHER_PRIORITY, why);
            return ERPS_E_REJECTED;
        }
        /* Rows 23/37/51 (MS in Protection / Manual switch / Forced switch) take no action:
         * an MS, FS or SF already exists in the ring (§10.2.3 rule 1). Reject with notice
         * instead of latching a command that would take effect much later. */
        if (erps__fsm_row_is_noop(e->state, ERPS_REQ_MS)) {
            reject(e, cmd, port, ERPS_REJECT_MS_IN_RING, why);
            return ERPS_E_REJECTED;
        }
        req.type = ERPS_REQ_MS;
        erps__log(e, ERPS_LOG_NOTICE, "manual switch on port %d", (int)port);
        evaluate(e, &req, false);
        if (e->state == ERPS_STATE_MANUAL_SWITCH) {
            e->ms_active = true;
            e->cmd_port = port;
        }
        return ERPS_OK;

    case ERPS_CMD_CLEAR: {
        /* §10.1.9: Clear is valid if a local FS/MS is in effect, or if this is the RPL owner
         * and the top-priority request is neither R-APS(FS) nor R-APS(MS). The node states
         * Manual-switch and Forced-switch exist only while an MS/FS is in effect in the ring, so
         * an owner in C or D without a local command is treated as having R-APS(MS/FS) on top
         * even if a lower message (e.g. a periodic R-APS(SF), row 49) was the last one signalled.
         * Otherwise row 44/30 would move the owner to Pending with no WTB running (no port
         * blocked) and nothing would ever revert the ring (found by the chaos test). */
        bool local_cmd = e->fs_active || e->ms_active;
        bool owner_ok = erps__is_owner(e) && e->top.type != ERPS_REQ_RAPS_FS &&
                        e->top.type != ERPS_REQ_RAPS_MS &&
                        e->state != ERPS_STATE_FORCED_SWITCH &&
                        e->state != ERPS_STATE_MANUAL_SWITCH;
        if (!local_cmd && !owner_ok) {
            reject(e, cmd, port, ERPS_REJECT_CLEAR_NOT_APPLICABLE, why);
            return ERPS_E_REJECTED;
        }
        e->fs_active = false;
        e->ms_active = false;
        req.type = ERPS_REQ_CLEAR;
        req.port = ERPS_PORT_NONE;
        erps__log(e, ERPS_LOG_NOTICE, "clear");
        evaluate(e, &req, false);
        return ERPS_OK;
    }
    }
    return ERPS_E_INVAL;
}
