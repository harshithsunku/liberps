/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Instance lifecycle and input plumbing:
 *   - validity check wiring (§10.1.6) and the R-APS receive pipeline of Figure 10-1:
 *       validity -> [flush logic] -> own-node filter -> guard timer -> priority logic;
 *   - local defect logic (§10.1.7) with hold-off timer (§10.1.8);
 *   - timer expiry and transmission scheduling (erps_poll / erps_next_deadline);
 *   - software R-APS forwarding (optional R-APS_FF for software dataplanes, §9.5).
 */
#include "erps_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------ */
/* Helpers                                                                                    */
/* ------------------------------------------------------------------------------------------ */

void erps__log(erps_t *e, erps_log_level_t lvl, const char *fmt, ...)
{
    if (!e->ops.log || lvl > e->cfg.log_level)
        return;
    char buf[320];
    int n = snprintf(buf, sizeof buf, "erps[%s] ", e->cfg.name);
    if (n < 0 || (size_t)n >= sizeof buf)
        n = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof buf - (size_t)n, fmt, ap);
    va_end(ap);
    e->ops.log(e->user, lvl, buf);
}

void erps__notify(erps_t *e, erps_event_t *ev)
{
    ev->time = e->now;
    if (e->ops.notify)
        e->ops.notify(e->user, ev);
}

void erps__platform_error(erps_t *e, const char *op, int rc, erps_port_t port)
{
    e->ctr.platform_errors++;
    erps__log(e, ERPS_LOG_ERR, "platform %s failed (rc=%d, port %d)", op, rc, (int)port);
    erps_event_t ev = { .type = ERPS_EV_PLATFORM_ERROR };
    ev.u.platform.op = op;
    ev.u.platform.rc = rc;
    ev.u.platform.port = port;
    erps__notify(e, &ev);
}

erps_time_t erps__deadline_after(const erps_t *e, uint64_t ms)
{
    return erps__tadd(e->now, ERPS_MSEC(ms));
}

/* True when every port the state machine wants blocked is blocked in the dataplane. New R-APS
 * information is never advertised before that holds: peers react to it by unblocking. */
bool erps__blocks_confirmed(const erps_t *e)
{
    for (int p = 0; p < 2; p++)
        if (e->port[p].blocked && !e->port[p].hw_synced)
            return false;
    return true;
}

static void sync_port(erps_t *e, erps_port_t p)
{
    struct erps_port *pp = &e->port[p];
    unsigned mask = pp->blocked ? (ERPS_BLOCK_TRAFFIC | ERPS_BLOCK_RAPS) : 0u;
    int rc = e->ops.set_port_state(e->user, p, mask);
    pp->hw_synced = rc == 0;
    if (rc) {
        erps__platform_error(e, "set_port_state", rc, p);
        e->retry_deadline = erps__tadd(e->now, ERPS_PLATFORM_RETRY_US);
    }
}

void erps__port_set(erps_t *e, erps_port_t p, bool blocked)
{
    struct erps_port *pp = &e->port[p];
    bool changed = pp->blocked != blocked;
    if (!changed && pp->hw_synced)
        return;
    pp->blocked = blocked;
    if (changed) {
        erps__trace_action(e, blocked ? (p == ERPS_PORT0 ? ERPS_ACT_BLOCK_P0 : ERPS_ACT_BLOCK_P1)
                                      : (p == ERPS_PORT0 ? ERPS_ACT_UNBLOCK_P0
                                                         : ERPS_ACT_UNBLOCK_P1));
        if (blocked)
            e->ctr.port[p].blocked++;
        else
            e->ctr.port[p].unblocked++;
        /* §10.1.10: when a ring port becomes blocked, both stored (node ID, BPR) pairs go. */
        if (blocked)
            erps__flush_clear_pairs(e);
        erps__log(e, ERPS_LOG_DEBUG, "port %d %s", (int)p, blocked ? "blocked" : "unblocked");
    }
    sync_port(e, p);
    if (changed) {
        erps_event_t ev = { .type = ERPS_EV_PORT_STATE };
        ev.u.port.port = p;
        ev.u.port.blocked = blocked;
        erps__notify(e, &ev);
    }
}

/* Every public entry point that mutates an instance goes through enter()/leave(). */
static erps_err_t enter(erps_t *e, erps_time_t now)
{
    if (!e || e->magic != ERPS_MAGIC)
        return ERPS_E_INVAL;
    if (e->depth)
        return ERPS_E_BUSY;
    e->depth++;
    if (e->now_valid && now < e->now) {
        e->ctr.clock_backwards++;
        now = e->now; /* never let time run backwards inside the state machine */
    }
    e->now = now;
    e->now_valid = true;
    return ERPS_OK;
}

static void really_destroy(erps_t *e);

static void leave(erps_t *e)
{
    erps__flush_commit(e);
    if (--e->depth == 0 && e->destroy_pending)
        really_destroy(e); /* destroyed from a callback: release now that nothing uses it */
}

static bool valid_port(erps_port_t p)
{
    return p == ERPS_PORT0 || p == ERPS_PORT1;
}

static void reset_runtime(erps_t *e)
{
    e->running = false;
    e->state = ERPS_STATE_INIT;
    e->fs_active = e->ms_active = false;
    e->cmd_port = ERPS_PORT_NONE;
    e->wtr_deadline = e->wtb_deadline = e->guard_deadline = ERPS_TIME_NEVER;
    e->retry_deadline = ERPS_TIME_NEVER;
    memset(&e->top, 0, sizeof e->top);
    e->top.port = ERPS_PORT_NONE;
    e->last_row = 0;
    e->state_changed = false;
    e->flush_pending = false;
    erps__tx_reset(e);
    erps__fop_reset(e);
    for (int p = 0; p < 2; p++) {
        struct erps_port *pp = &e->port[p];
        pp->sf = false;
        pp->holdoff_deadline = ERPS_TIME_NEVER;
        pp->pair_valid = false;
        memset(pp->pair_node, 0, sizeof pp->pair_node); /* §10.1.10: (00:..:00, 0) */
        pp->pair_bpr = 0;
        pp->rx_seen = false;
        pp->last_rx_time = 0;
        memset(&pp->last_rx, 0, sizeof pp->last_rx);
    }
    e->flush_retry = false;
    e->flush_ind_until = 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Lifecycle                                                                                  */
/* ------------------------------------------------------------------------------------------ */

size_t erps_instance_size(void)
{
    return sizeof(struct erps);
}

erps_err_t erps_init_static(void *mem, size_t mem_len, const erps_config_t *cfg,
                            const erps_ops_t *ops, void *user, erps_t **out)
{
    if (!mem || !cfg || !ops || !out)
        return ERPS_E_INVAL;
    if (mem_len < sizeof(struct erps))
        return ERPS_E_NOMEM;
    if (!ops->tx || !ops->set_port_state || !ops->flush_fdb)
        return ERPS_E_INVAL;
    erps_err_t rc = erps_config_validate(cfg, NULL, 0);
    if (rc != ERPS_OK)
        return rc;

    erps_t *e = mem;
    memset(e, 0, sizeof *e);
    e->magic = ERPS_MAGIC;
    e->cfg = *cfg;
    e->cfg.name[ERPS_NAME_MAX - 1] = '\0';
    for (int p = 0; p < 2; p++)
        e->cfg.port[p].name[ERPS_PORT_NAME_MAX - 1] = '\0';
    if (e->cfg.history_depth == 0 || e->cfg.history_depth > ERPS_HISTORY_MAX)
        e->cfg.history_depth = ERPS_HISTORY_MAX;
    e->ops = *ops;
    e->user = user;
    reset_runtime(e);
    *out = e;
    return ERPS_OK;
}

erps_err_t erps_create(const erps_config_t *cfg, const erps_ops_t *ops, void *user, erps_t **out)
{
    if (!out)
        return ERPS_E_INVAL;
    void *mem = malloc(sizeof(struct erps));
    if (!mem)
        return ERPS_E_NOMEM;
    erps_err_t rc = erps_init_static(mem, sizeof(struct erps), cfg, ops, user, out);
    if (rc != ERPS_OK) {
        free(mem);
        return rc;
    }
    (*out)->heap = true;
    return ERPS_OK;
}

static void really_destroy(erps_t *e)
{
    bool heap = e->heap;
    e->magic = 0;
    if (heap)
        free(e);
}

void erps_destroy(erps_t *e)
{
    if (!e || e->magic != ERPS_MAGIC)
        return;
    if (e->depth) { /* called from one of our callbacks: defer until the call unwinds */
        e->destroy_pending = true;
        return;
    }
    really_destroy(e);
}

static void start_holdoff_or_declare(erps_t *e, erps_port_t p);

erps_err_t erps_start(erps_t *e, erps_time_t now)
{
    erps_err_t rc = enter(e, now);
    if (rc != ERPS_OK)
        return rc;
    if (e->running) {
        leave(e);
        return ERPS_E_STATE;
    }
    reset_runtime(e);
    e->running = true;
    e->port[0].hw_synced = e->port[1].hw_synced = false; /* program both ports explicitly */
    erps__log(e, ERPS_LOG_NOTICE, "start: ring %u vid %u mel %u role %s rpl-port %d %s v%u",
              e->cfg.ring_id, e->cfg.raps_vid, e->cfg.mel, erps_role_str(e->cfg.role),
              (int)e->cfg.rpl_port, erps__revertive(e) ? "revertive" : "non-revertive",
              e->cfg.compat_version);
    erps__fop_reset(e);
    erps__fsm_init(e);
    /* Signal fail present before start goes through the hold-off timer like any new SF. */
    for (int p = 0; p < 2; p++)
        if (e->port[p].ssf)
            start_holdoff_or_declare(e, (erps_port_t)p);
    leave(e);
    return ERPS_OK;
}

erps_err_t erps_stop(erps_t *e, erps_time_t now, bool block_ports)
{
    erps_err_t rc = enter(e, now);
    if (rc != ERPS_OK)
        return rc;
    if (e->running) {
        if (block_ports) {
            erps__port_set(e, ERPS_PORT0, true);
            erps__port_set(e, ERPS_PORT1, true);
            if (!erps__blocks_confirmed(e))
                rc = ERPS_E_PLATFORM; /* the caller must not assume the ring is safe */
        }
        erps_state_t from = e->state;
        reset_runtime(e);
        erps__log(e, ERPS_LOG_NOTICE, "stop%s", block_ports ? " (ring ports blocked)" : "");
        erps_event_t ev = { .type = ERPS_EV_STATE_CHANGE };
        ev.u.state.from = from;
        ev.u.state.to = ERPS_STATE_INIT;
        erps__notify(e, &ev);
    }
    leave(e);
    return rc;
}

erps_err_t erps_set_timers(erps_t *e, uint32_t wtr_ms, uint32_t guard_ms, uint32_t holdoff_ms)
{
    if (!e || e->magic != ERPS_MAGIC)
        return ERPS_E_INVAL;
    erps_config_t c = e->cfg;
    c.wtr_ms = wtr_ms;
    c.guard_ms = guard_ms;
    c.holdoff_ms = holdoff_ms;
    erps_err_t rc = erps_config_validate(&c, NULL, 0);
    if (rc != ERPS_OK)
        return rc;
    e->cfg.wtr_ms = wtr_ms;
    e->cfg.guard_ms = guard_ms;
    e->cfg.holdoff_ms = holdoff_ms;
    return ERPS_OK;
}

erps_err_t erps_set_revertive(erps_t *e, bool revertive)
{
    if (!e || e->magic != ERPS_MAGIC)
        return ERPS_E_INVAL;
    if (e->depth)
        return ERPS_E_BUSY;
    e->cfg.revertive = revertive;
    /* Going non-revertive cancels a pending automatic reversion (§10.2.2.2: only an operator
     * Clear reverts). Going revertive takes effect at the next recovery event, or use Clear. */
    if (!erps__revertive(e) && (e->wtr_deadline != ERPS_TIME_NEVER ||
                                e->wtb_deadline != ERPS_TIME_NEVER)) {
        e->wtr_deadline = e->wtb_deadline = ERPS_TIME_NEVER;
        erps__log(e, ERPS_LOG_NOTICE, "non-revertive: WTR/WTB stopped, use Clear to revert");
    }
    return ERPS_OK;
}

void erps_set_log_level(erps_t *e, erps_log_level_t level)
{
    if (e && e->magic == ERPS_MAGIC && level <= ERPS_LOG_TRACE)
        e->cfg.log_level = level;
}

const erps_config_t *erps_get_config(const erps_t *e)
{
    return (e && e->magic == ERPS_MAGIC) ? &e->cfg : NULL;
}

void *erps_get_user(const erps_t *e)
{
    return (e && e->magic == ERPS_MAGIC) ? e->user : NULL;
}

/* ------------------------------------------------------------------------------------------ */
/* Receive path                                                                               */
/* ------------------------------------------------------------------------------------------ */

static erps_err_t drop(erps_t *e, erps_port_t p, erps_drop_t why)
{
    e->ctr.port[p].rx_drop[why]++;
    erps__log(e, ERPS_LOG_DEBUG, "rx port %d dropped: %s", (int)p, erps_drop_str(why));
    return ERPS_E_DROPPED;
}

/* Software R-APS_FF (§9.5): forward between ring ports unless either is blocked. A frame
 * carrying our own node ID has travelled the whole ring and is not forwarded again. */
static void sw_forward(erps_t *e, erps_port_t in, const erps_rx_meta_t *meta,
                       const uint8_t *frame, size_t flen, const uint8_t *pdu, size_t plen)
{
    erps_port_t out = erps__other(in);
    if (e->port[in].blocked || e->port[out].blocked)
        return;
    int rc;
    if (frame) {
        rc = e->ops.tx(e->user, out, frame, flen);
    } else {
        uint8_t buf[1536];
        if (plen > sizeof buf - 18)
            return;
        memcpy(buf, meta->dst, 6);
        memcpy(buf + 6, meta->src, 6);
        buf[12] = 0x81;
        buf[13] = 0x00;
        buf[14] = (uint8_t)(((meta->pcp & 7u) << 5) | ((meta->vid >> 8) & 0x0Fu));
        buf[15] = (uint8_t)(meta->vid & 0xFFu);
        buf[16] = 0x89;
        buf[17] = 0x02;
        memcpy(buf + 18, pdu, plen);
        size_t n = 18 + plen;
        while (n < ERPS_FRAME_MIN_LEN)
            buf[n++] = 0;
        rc = e->ops.tx(e->user, out, buf, n);
    }
    if (rc)
        erps__platform_error(e, "tx(forward)", rc, out);
    else
        e->ctr.port[in].sw_forwarded++;
}

static erps_req_type_t raps_req_type(const erps_raps_msg_t *m)
{
    switch (m->req) {
    case ERPS_RAPS_NR:
        return m->rb ? ERPS_REQ_RAPS_NR_RB : ERPS_REQ_RAPS_NR;
    case ERPS_RAPS_SF:
        return ERPS_REQ_RAPS_SF;
    case ERPS_RAPS_MS:
        return ERPS_REQ_RAPS_MS;
    case ERPS_RAPS_FS:
        return ERPS_REQ_RAPS_FS;
    default:
        return ERPS_REQ_NONE;
    }
}

static erps_err_t rx_common(erps_t *e, erps_port_t p, const erps_rx_meta_t *meta,
                            const uint8_t *frame, size_t flen, const uint8_t *pdu, size_t plen)
{
    uint8_t ring_id;
    if (!erps_is_raps_dmac(meta->dst, &ring_id))
        return drop(e, p, ERPS_DROP_DA);
    if (plen < ERPS_PDU_HDR_LEN)
        return drop(e, p, ERPS_DROP_SHORT);

    /* Software R-APS_FF (§9.5): forwarding on the R-APS VLAN is independent of this control
     * process' validity check, like a hardware flow-forwarding function. MEG level filtering
     * still applies: a frame below the ring MEL is discarded (G.8021 ETHDi). */
    uint8_t mel = (uint8_t)(pdu[0] >> 5);
    if (mel < e->cfg.mel)
        return drop(e, p, ERPS_DROP_MEL);
    bool own = pdu[1] == ERPS_OPCODE_RAPS && plen >= 12 && memcmp(pdu + 6, e->cfg.node_id, 6) == 0;
    if (e->cfg.sw_raps_forwarding && !own)
        sw_forward(e, p, meta, frame, flen, pdu, plen);

    /* Validity check (§10.1.6) for this control process */
    if (ring_id != e->cfg.ring_id)
        return drop(e, p, ERPS_DROP_RING_ID);
    if (mel != e->cfg.mel)
        return drop(e, p, ERPS_DROP_MEL);
    erps_pdu_t pd;
    erps_drop_t why = erps_pdu_decode(pdu, plen, &pd);
    if (why != ERPS_DROP_NONE)
        return drop(e, p, why);

    const erps_raps_msg_t *m = &pd.msg;
    erps_msg_kind_t kind = erps_msg_kind(m);
    if (kind < ERPS_MSG_COUNT)
        e->ctr.port[p].rx_raps[kind]++;
    struct erps_port *pp = &e->port[p];
    pp->rx_seen = true;
    pp->last_rx_time = e->now;
    pp->last_rx = *m;
    erps__fop_rx(e, p, m);

    if (m->req == ERPS_RAPS_EVENT) {
        /* Flush indication (§10.1.6): signalled to the flush logic, not subject to the guard
         * timer, and disabled after 10 ms — the three copies of one burst flush once. */
        if (e->now >= e->flush_ind_until) {
            erps__log(e, ERPS_LOG_DEBUG, "rx port %d: flush event", (int)p);
            erps__flush_request(e);
        }
        e->flush_ind_until = erps__tadd(e->now, ERPS_MSEC(10));
        return ERPS_OK;
    }

    if (e->ops.log && e->cfg.log_level >= ERPS_LOG_TRACE) {
        char b[96];
        erps_raps_msg_format(m, b, sizeof b);
        erps__log(e, ERPS_LOG_TRACE, "rx port %d: %s", (int)p, b);
    }

    erps__flush_rx(e, p, m); /* flush logic sees every valid message (Figure 10-1) */

    if (own) { /* §10.1.1: own node ID is ignored by the priority logic */
        e->ctr.rx_own++;
        return ERPS_OK;
    }
    if (e->guard_deadline != ERPS_TIME_NEVER) { /* §10.1.5 */
        e->ctr.rx_guard++;
        return ERPS_OK;
    }

    erps_req_t req;
    memset(&req, 0, sizeof req);
    req.type = raps_req_type(m);
    req.port = p;
    req.raps = *m;
    erps__prio_raps(e, &req);
    return ERPS_OK;
}

erps_err_t erps_rx_frame(erps_t *e, erps_port_t port, const uint8_t *frame, size_t len,
                         erps_time_t now)
{
    if (!valid_port(port) || (!frame && len))
        return ERPS_E_INVAL;
    erps_err_t rc = enter(e, now);
    if (rc != ERPS_OK)
        return rc;
    e->ctr.port[port].rx_frames++;
    erps_eth_t eth;
    if (!e->running)
        rc = drop(e, port, ERPS_DROP_NOT_RUNNING);
    else if (erps_eth_parse(frame, len, &eth) != ERPS_DROP_NONE)
        rc = drop(e, port, ERPS_DROP_SHORT);
    else if (!eth.tagged || eth.vid != e->cfg.raps_vid)
        rc = drop(e, port, ERPS_DROP_VID);
    else if (eth.ethertype != ERPS_ETHERTYPE_CFM)
        rc = drop(e, port, ERPS_DROP_ETHERTYPE);
    else {
        erps_rx_meta_t meta;
        memcpy(meta.dst, eth.dst, 6);
        memcpy(meta.src, eth.src, 6);
        meta.vid = eth.vid;
        meta.pcp = eth.pcp;
        rc = rx_common(e, port, &meta, frame, len, frame + eth.hdr_len, len - eth.hdr_len);
    }
    leave(e);
    return rc;
}

erps_err_t erps_rx_pdu(erps_t *e, erps_port_t port, const erps_rx_meta_t *meta,
                       const uint8_t *pdu, size_t len, erps_time_t now)
{
    if (!valid_port(port) || !meta || (!pdu && len))
        return ERPS_E_INVAL;
    erps_err_t rc = enter(e, now);
    if (rc != ERPS_OK)
        return rc;
    e->ctr.port[port].rx_frames++;
    if (!e->running)
        rc = drop(e, port, ERPS_DROP_NOT_RUNNING);
    else if (meta->vid != e->cfg.raps_vid)
        rc = drop(e, port, ERPS_DROP_VID);
    else
        rc = rx_common(e, port, meta, NULL, 0, pdu, len);
    leave(e);
    return rc;
}

/* ------------------------------------------------------------------------------------------ */
/* Local defect logic (§10.1.7) and hold-off timer (§10.1.8)                                   */
/* ------------------------------------------------------------------------------------------ */

static void declare_sf(erps_t *e, erps_port_t p)
{
    struct erps_port *pp = &e->port[p];
    pp->holdoff_deadline = ERPS_TIME_NEVER;
    if (pp->sf)
        return;
    pp->sf = true;
    erps__log(e, ERPS_LOG_NOTICE, "port %d: signal fail", (int)p);
    erps_event_t ev = { .type = ERPS_EV_SF };
    ev.u.sf.port = p;
    ev.u.sf.sf = true;
    erps__notify(e, &ev);
    if (e->running)
        erps__prio_event(e, NULL); /* persistent request changed */
}

static void clear_sf(erps_t *e, erps_port_t p)
{
    struct erps_port *pp = &e->port[p];
    pp->holdoff_deadline = ERPS_TIME_NEVER;
    if (!pp->sf)
        return;
    pp->sf = false;
    erps__log(e, ERPS_LOG_NOTICE, "port %d: signal fail cleared", (int)p);
    erps_event_t ev = { .type = ERPS_EV_SF };
    ev.u.sf.port = p;
    ev.u.sf.sf = false;
    erps__notify(e, &ev);
    if (e->running) {
        erps_req_t req;
        memset(&req, 0, sizeof req);
        req.type = ERPS_REQ_LOCAL_CLEAR_SF;
        req.port = p;
        erps__prio_event(e, &req);
    }
}

static void start_holdoff_or_declare(erps_t *e, erps_port_t p)
{
    if (e->cfg.holdoff_ms == 0)
        declare_sf(e, p);
    else if (e->port[p].holdoff_deadline == ERPS_TIME_NEVER && !e->port[p].sf)
        e->port[p].holdoff_deadline = erps__deadline_after(e, e->cfg.holdoff_ms);
}

erps_err_t erps_set_signal_fail(erps_t *e, erps_port_t port, bool ssf, erps_time_t now)
{
    if (!valid_port(port))
        return ERPS_E_INVAL;
    erps_err_t rc = enter(e, now);
    if (rc != ERPS_OK)
        return rc;
    struct erps_port *pp = &e->port[port];
    if (pp->ssf != ssf) {
        pp->ssf = ssf;
        erps__log(e, ERPS_LOG_DEBUG, "port %d: SSF %s", (int)port, ssf ? "set" : "cleared");
        if (e->running) {
            if (ssf)
                start_holdoff_or_declare(e, port);
            else
                clear_sf(e, port); /* also cancels a running hold-off timer */
        }
    }
    leave(e);
    return ERPS_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Commands                                                                                   */
/* ------------------------------------------------------------------------------------------ */

erps_err_t erps_command(erps_t *e, erps_cmd_t cmd, erps_port_t port, erps_time_t now,
                        erps_reject_t *reason)
{
    if (cmd != ERPS_CMD_CLEAR && !valid_port(port))
        return ERPS_E_INVAL;
    if (cmd > ERPS_CMD_CLEAR)
        return ERPS_E_INVAL;
    erps_err_t rc = enter(e, now);
    if (rc != ERPS_OK)
        return rc;
    rc = erps__prio_command(e, cmd, cmd == ERPS_CMD_CLEAR ? ERPS_PORT_NONE : port, reason);
    leave(e);
    return rc;
}

erps_err_t erps_send_flush_event(erps_t *e, erps_time_t now)
{
    erps_err_t rc = enter(e, now);
    if (rc != ERPS_OK)
        return rc;
    if (!e->running)
        rc = ERPS_E_STATE;
    else
        erps__tx_event(e);
    leave(e);
    return rc;
}

/* ------------------------------------------------------------------------------------------ */
/* Time                                                                                       */
/* ------------------------------------------------------------------------------------------ */

static erps_time_t min_t(erps_time_t a, erps_time_t b)
{
    return a < b ? a : b;
}

static erps_time_t timers_next(const erps_t *e)
{
    erps_time_t t = min_t(e->wtr_deadline, e->wtb_deadline);
    t = min_t(t, e->guard_deadline);
    t = min_t(t, e->retry_deadline);
    t = min_t(t, e->port[0].holdoff_deadline);
    t = min_t(t, e->port[1].holdoff_deadline);
    return t;
}

erps_time_t erps_next_deadline(const erps_t *e)
{
    if (!e || e->magic != ERPS_MAGIC || !e->running)
        return ERPS_TIME_NEVER;
    erps_time_t t = timers_next(e);
    t = min_t(t, erps__tx_next(e));
    t = min_t(t, erps__fop_next(e));
    return t;
}

/* Fire the earliest expired protocol timer. Returns false when none is due. */
static bool fire_one_timer(erps_t *e)
{
    erps_time_t now = e->now;
    erps_time_t best = ERPS_TIME_NEVER;
    int which = -1;
    const erps_time_t cand[6] = { e->port[0].holdoff_deadline, e->port[1].holdoff_deadline,
                                  e->guard_deadline, e->wtr_deadline, e->wtb_deadline,
                                  e->retry_deadline };
    for (int i = 0; i < 6; i++) {
        if (cand[i] <= now && cand[i] < best) {
            best = cand[i];
            which = i;
        }
    }
    erps_req_t req;
    memset(&req, 0, sizeof req);
    req.port = ERPS_PORT_NONE;
    switch (which) {
    case 0:
    case 1: {
        erps_port_t p = (erps_port_t)which;
        e->port[p].holdoff_deadline = ERPS_TIME_NEVER;
        if (e->port[p].ssf) /* §10.1.8: report only if the defect still exists */
            declare_sf(e, p);
        return true;
    }
    case 2: /* guard timer expires by itself (§10.1.5) */
        e->guard_deadline = ERPS_TIME_NEVER;
        erps__log(e, ERPS_LOG_DEBUG, "guard timer expired");
        return true;
    case 3:
        e->wtr_deadline = ERPS_TIME_NEVER;
        erps__log(e, ERPS_LOG_INFO, "WTR expired");
        req.type = ERPS_REQ_WTR_EXPIRES;
        erps__prio_event(e, &req);
        return true;
    case 4:
        e->wtb_deadline = ERPS_TIME_NEVER;
        erps__log(e, ERPS_LOG_INFO, "WTB expired");
        req.type = ERPS_REQ_WTB_EXPIRES;
        erps__prio_event(e, &req);
        return true;
    case 5:
        e->retry_deadline = ERPS_TIME_NEVER;
        for (int p = 0; p < 2; p++)
            if (!e->port[p].hw_synced)
                sync_port(e, (erps_port_t)p);
        if (e->flush_retry) {
            e->flush_retry = false;
            erps__flush_request(e); /* committed when this poll completes */
        }
        return true;
    default:
        return false;
    }
}

erps_err_t erps_poll(erps_t *e, erps_time_t now)
{
    erps_err_t rc = enter(e, now);
    if (rc != ERPS_OK)
        return rc;
    if (e->running) {
        /* Timers in deadline order; bounded in case a callback keeps re-arming the retry. */
        for (int i = 0; i < 32 && fire_one_timer(e); i++)
            ;
        erps__fop_poll(e);
        erps__tx_poll(e);
    }
    leave(e);
    return ERPS_OK;
}

void erps__test_fsm_run(erps_t *e, const erps_req_t *req, erps_time_t now)
{
    if (enter(e, now) != ERPS_OK)
        return;
    erps__fsm_run(e, req);
    leave(e);
}
