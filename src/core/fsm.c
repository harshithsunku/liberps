/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R-APS request processing — G.8032 §10.1.2, Table 10-2.
 *
 * The table below is a literal transcription of Table 10-2: one entry per (node state, top
 * priority request) with the spec's row number, an action function and the next node state.
 * Action functions are named after the action lists they implement and are shared by rows
 * with identical lists. Row numbers flow into the transition history and row-coverage
 * bitmap, so a trace line can be checked directly against the Recommendation.
 *
 * Action vocabulary (§10.1.2 a–r) is implemented by the small helpers in the next section.
 */
#include "erps_internal.h"

#include <string.h>

/* ------------------------------------------------------------------------------------------ */
/* Actions a)–r)                                                                               */
/* ------------------------------------------------------------------------------------------ */

static void block(erps_t *e, erps_port_t p)
{
    erps__port_set(e, p, true);
}

static void unblock(erps_t *e, erps_port_t p)
{
    erps__port_set(e, p, false);
}

/* a) Block requested ring port */
static void block_requested(erps_t *e, const erps_req_t *r)
{
    block(e, r->port);
}

/* b) Unblock non-requested ring port */
static void unblock_non_requested(erps_t *e, const erps_req_t *r)
{
    unblock(e, erps__other(r->port));
}

/* c) Block failed ring port (every port with a declared SF) */
static void block_failed(erps_t *e)
{
    for (int p = 0; p < 2; p++)
        if (e->port[p].sf)
            block(e, (erps_port_t)p);
}

/* d) Unblock non-failed ring port (every port without SF) */
static void unblock_non_failed(erps_t *e)
{
    for (int p = 0; p < 2; p++)
        if (!e->port[p].sf)
            unblock(e, (erps_port_t)p);
}

/* e) Block RPL port */
static void block_rpl(erps_t *e)
{
    block(e, e->cfg.rpl_port);
}

/* f) Unblock non-RPL port: every ring port that is not the RPL port. A node without an RPL
 *    role has no RPL port, so both of its ports are non-RPL ports. */
static void unblock_non_rpl(erps_t *e)
{
    for (int p = 0; p < 2; p++) {
        bool is_rpl = e->cfg.role != ERPS_ROLE_NONE && (erps_port_t)p == e->cfg.rpl_port;
        if (!is_rpl)
            unblock(e, (erps_port_t)p);
    }
}

/* g) Block one ring port / h) Unblock other ring port. Port 0 is "the one" (deterministic). */
static void block_one(erps_t *e)
{
    block(e, ERPS_PORT0);
}

static void unblock_other(erps_t *e)
{
    unblock(e, ERPS_PORT1);
}

/* i) Unblock ring ports */
static void unblock_ring_ports(erps_t *e)
{
    unblock(e, ERPS_PORT0);
    unblock(e, ERPS_PORT1);
}

/* j)–m) WTR / WTB: start does not restart a running timer; stop resets it, no Expires. */
static void start_wtr(erps_t *e)
{
    if (e->wtr_deadline == ERPS_TIME_NEVER) {
        e->wtr_deadline = erps__deadline_after(e, e->cfg.wtr_ms);
        erps__trace_action(e, ERPS_ACT_START_WTR);
    }
}

static void stop_wtr(erps_t *e)
{
    if (e->wtr_deadline != ERPS_TIME_NEVER) {
        e->wtr_deadline = ERPS_TIME_NEVER;
        erps__trace_action(e, ERPS_ACT_STOP_WTR);
    }
}

static void start_wtb(erps_t *e)
{
    if (e->wtb_deadline == ERPS_TIME_NEVER) {
        e->wtb_deadline = erps__deadline_after(e, (uint64_t)e->cfg.guard_ms + ERPS_WTB_EXTRA_MS);
        erps__trace_action(e, ERPS_ACT_START_WTB);
    }
}

static void stop_wtb(erps_t *e)
{
    if (e->wtb_deadline != ERPS_TIME_NEVER) {
        e->wtb_deadline = ERPS_TIME_NEVER;
        erps__trace_action(e, ERPS_ACT_STOP_WTB);
    }
}

/* n) Start guard timer — (re)starts it. o) Stop guard timer. */
static void start_guard(erps_t *e)
{
    e->guard_deadline = erps__deadline_after(e, e->cfg.guard_ms);
    erps__trace_action(e, ERPS_ACT_START_GUARD);
}

static void stop_guard(erps_t *e)
{
    if (e->guard_deadline != ERPS_TIME_NEVER) {
        e->guard_deadline = ERPS_TIME_NEVER;
        erps__trace_action(e, ERPS_ACT_STOP_GUARD);
    }
}

/* p) Stop Tx R-APS */
static void stop_tx(erps_t *e)
{
    erps__tx_stop(e);
}

/* r) Flush FDB */
static void flush(erps_t *e)
{
    erps__trace_action(e, ERPS_ACT_FLUSH);
    erps__flush_request(e);
}

/*
 * Blocked port reference to transmit (§10.3 h3): the port this row is about if it is blocked,
 * otherwise the only blocked port, otherwise 0 ("if two ring links are blocked, the encoded
 * value can be either value").
 */
static uint8_t bpr_for(const erps_t *e, erps_port_t subject)
{
    bool b0 = e->port[0].blocked, b1 = e->port[1].blocked;
    if (subject != ERPS_PORT_NONE && e->port[subject].blocked)
        return (uint8_t)subject;
    if (b1 && !b0)
        return 1;
    return 0;
}

/* q) Tx R-APS (msgtype, status_bits) */
static void tx(erps_t *e, uint8_t code, bool rb, bool dnf, erps_port_t subject)
{
    erps_raps_msg_t m;
    memset(&m, 0, sizeof m);
    m.req = code;
    m.rb = rb;
    m.dnf = dnf;
    m.bpr = bpr_for(e, subject);
    m.status = erps_status_octet(rb, dnf, m.bpr);
    memcpy(m.node_id, e->cfg.node_id, 6);
    erps__tx_start(e, &m);
}

static bool any_port_blocked(const erps_t *e)
{
    return e->port[0].blocked || e->port[1].blocked;
}

static bool both_ports_unblocked(const erps_t *e)
{
    return !e->port[0].blocked && !e->port[1].blocked;
}

/* The "failed ring port" of a local SF request: all ports with SF; the subject for BPR is the
 * port named by the request (most recent failure) when it is still failed. */
static bool failed_ports_all_blocked(const erps_t *e)
{
    bool any = false;
    for (int p = 0; p < 2; p++) {
        if (e->port[p].sf) {
            any = true;
            if (!e->port[p].blocked)
                return false;
        }
    }
    return any;
}

static erps_port_t failed_subject(const erps_t *e, const erps_req_t *r)
{
    if (r->port != ERPS_PORT_NONE && e->port[r->port].sf)
        return r->port;
    if (e->port[0].sf)
        return ERPS_PORT0;
    if (e->port[1].sf)
        return ERPS_PORT1;
    return ERPS_PORT_NONE;
}

static int node_id_higher_than_own(const erps_t *e, const erps_req_t *r)
{
    return erps_node_id_cmp(r->raps.node_id, e->cfg.node_id) > 0;
}

/* ------------------------------------------------------------------------------------------ */
/* Row action lists                                                                            */
/* ------------------------------------------------------------------------------------------ */

typedef erps_state_t (*fsm_fn)(erps_t *e, const erps_req_t *r, erps_state_t next);

static erps_state_t no_action(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)e;
    (void)r;
    return next;
}

/* Rows 3, 17, 31 (FS) and 9 (MS): operator switch on a requested port. */
static void switch_to_requested(erps_t *e, const erps_req_t *r, uint8_t code)
{
    if (e->port[r->port].blocked) {
        tx(e, code, false, true, r->port);
        unblock_non_requested(e, r);
    } else {
        block_requested(e, r);
        tx(e, code, false, false, r->port);
        unblock_non_requested(e, r);
        flush(e);
    }
}

/* Rows 3, 17, 31 */
static erps_state_t fs_switch(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    switch_to_requested(e, r, ERPS_RAPS_FS);
    return next;
}

/* Row 9 */
static erps_state_t ms_switch(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    switch_to_requested(e, r, ERPS_RAPS_MS);
    return next;
}

/* Row 59: as row 3, then if owner stop WTR/WTB */
static erps_state_t fs_switch_from_pending(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    switch_to_requested(e, r, ERPS_RAPS_FS);
    if (erps__is_owner(e)) {
        stop_wtr(e);
        stop_wtb(e);
    }
    return next;
}

/* Row 65: if owner stop WTR/WTB, then as row 9 */
static erps_state_t ms_switch_from_pending(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    if (erps__is_owner(e)) {
        stop_wtr(e);
        stop_wtb(e);
    }
    switch_to_requested(e, r, ERPS_RAPS_MS);
    return next;
}

/* Row 45: additional FS on a node already in FS state */
static erps_state_t fs_in_fs(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    block_requested(e, r);
    tx(e, ERPS_RAPS_FS, false, false, r->port);
    flush(e);
    return next;
}

/* Rows 4, 18, 32: R-APS(FS) */
static erps_state_t raps_fs(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    unblock_ring_ports(e);
    stop_tx(e);
    return next;
}

/* Row 60 */
static erps_state_t raps_fs_from_pending(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    raps_fs(e, r, next);
    if (erps__is_owner(e)) {
        stop_wtr(e);
        stop_wtb(e);
    }
    return next;
}

/* Rows 5, 19, 33: local SF */
static void local_sf_actions(erps_t *e, const erps_req_t *r)
{
    erps_port_t subject = failed_subject(e, r);
    if (failed_ports_all_blocked(e)) {
        tx(e, ERPS_RAPS_SF, false, true, subject);
        unblock_non_failed(e);
    } else {
        block_failed(e);
        tx(e, ERPS_RAPS_SF, false, false, subject);
        unblock_non_failed(e);
        flush(e);
    }
}

static erps_state_t local_sf(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    local_sf_actions(e, r);
    return next;
}

/* Row 61 */
static erps_state_t local_sf_from_pending(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    local_sf_actions(e, r);
    if (erps__is_owner(e)) {
        stop_wtr(e);
        stop_wtb(e);
    }
    return next;
}

/* Rows 7, 8, 35: R-APS(SF) / R-APS(MS) accepted */
static erps_state_t unblock_non_failed_stop_tx(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    unblock_non_failed(e);
    stop_tx(e);
    return next;
}

/* Rows 63, 64 */
static erps_state_t unblock_non_failed_stop_tx_from_pending(erps_t *e, const erps_req_t *r,
                                                            erps_state_t next)
{
    unblock_non_failed_stop_tx(e, r, next);
    if (erps__is_owner(e)) {
        stop_wtr(e);
        stop_wtb(e);
    }
    return next;
}

/* Row 14: R-APS(NR,RB) in Idle */
static erps_state_t raps_nr_rb_idle(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    unblock_non_rpl(e);
    if (!erps__is_owner(e))
        stop_tx(e);
    return next;
}

/* Row 15: R-APS(NR) in Idle */
static erps_state_t raps_nr_idle(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    if (e->cfg.role == ERPS_ROLE_NONE && node_id_higher_than_own(e, r)) {
        unblock_non_failed(e);
        stop_tx(e);
    }
    return next;
}

/* Row 20: local clear SF in Protection */
static erps_state_t clear_sf_protection(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    start_guard(e);
    tx(e, ERPS_RAPS_NR, false, false, r->port);
    if (erps__is_owner(e) && erps__revertive(e))
        start_wtr(e);
    return next;
}

/* Row 29: R-APS(NR) in Protection */
static erps_state_t owner_start_wtr(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    if (erps__is_owner(e) && erps__revertive(e))
        start_wtr(e);
    return next;
}

/* Rows 43, 57: R-APS(NR) in MS / FS */
static erps_state_t owner_start_wtb(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    if (erps__is_owner(e) && erps__revertive(e))
        start_wtb(e);
    return next;
}

/* Rows 30, 44: Clear in MS / FS (the node keeps its port blocked, §10.2.3.1 / §10.2.4.1) */
static erps_state_t clear_command(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    if (any_port_blocked(e)) {
        start_guard(e);
        tx(e, ERPS_RAPS_NR, false, false, ERPS_PORT_NONE);
        if (erps__is_owner(e) && erps__revertive(e))
            start_wtb(e);
    }
    return next;
}

/* Row 36: R-APS(MS) in MS. Footnote a): if both ring ports are unblocked, stay in C. */
static erps_state_t raps_ms_in_ms(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    if (both_ports_unblocked(e))
        return ERPS_STATE_MANUAL_SWITCH;
    return clear_command(e, r, next);
}

/* Reversion by the RPL owner (rows 58, 66, 68): block the RPL and announce (NR,RB). */
static void owner_revert(erps_t *e)
{
    if (e->port[e->cfg.rpl_port].blocked) {
        tx(e, ERPS_RAPS_NR, true, true, e->cfg.rpl_port);
        unblock_non_rpl(e);
    } else {
        block_rpl(e);
        tx(e, ERPS_RAPS_NR, true, false, e->cfg.rpl_port);
        unblock_non_rpl(e);
        flush(e);
    }
}

/* Row 58: Clear in Pending (owner only — the local priority logic admits no other Clear) */
static erps_state_t clear_pending(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    if (erps__is_owner(e)) {
        stop_wtr(e);
        stop_wtb(e);
        owner_revert(e);
    }
    return next;
}

/* Row 66: WTR Expires */
static erps_state_t wtr_expires(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    if (erps__is_owner(e)) {
        stop_wtb(e);
        owner_revert(e);
    }
    return next;
}

/* Row 68: WTB Expires */
static erps_state_t wtb_expires(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    if (erps__is_owner(e)) {
        stop_wtr(e);
        owner_revert(e);
    }
    return next;
}

/* Row 70: R-APS(NR,RB) in Pending */
static erps_state_t raps_nr_rb_pending(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    (void)r;
    if (erps__is_owner(e)) {
        stop_wtr(e);
        stop_wtb(e);
    }
    if (e->cfg.role == ERPS_ROLE_NONE) {
        unblock_ring_ports(e);
        stop_tx(e);
    }
    if (erps__is_neighbour(e)) {
        block_rpl(e);
        unblock_non_rpl(e);
        stop_tx(e);
    }
    return next;
}

/* Row 71: R-APS(NR) in Pending */
static erps_state_t raps_nr_pending(erps_t *e, const erps_req_t *r, erps_state_t next)
{
    if (node_id_higher_than_own(e, r)) {
        unblock_non_failed(e);
        stop_tx(e);
    }
    return next;
}

/* ------------------------------------------------------------------------------------------ */
/* Table 10-2                                                                                  */
/* ------------------------------------------------------------------------------------------ */

struct fsm_row {
    uint8_t row;
    fsm_fn fn;
    erps_state_t next;
};

#define A ERPS_STATE_IDLE
#define B ERPS_STATE_PROTECTION
#define C ERPS_STATE_MANUAL_SWITCH
#define D ERPS_STATE_FORCED_SWITCH
#define E ERPS_STATE_PENDING

#define R(n, f, s) { (n), (f), (s) }

static const struct fsm_row fsm_table[ERPS_STATE_COUNT][ERPS_REQ_COUNT] = {
    [ERPS_STATE_IDLE] = {
        [ERPS_REQ_CLEAR]          = R( 2, no_action,                 A),
        [ERPS_REQ_FS]             = R( 3, fs_switch,                 D),
        [ERPS_REQ_RAPS_FS]        = R( 4, raps_fs,                   D),
        [ERPS_REQ_LOCAL_SF]       = R( 5, local_sf,                  B),
        [ERPS_REQ_LOCAL_CLEAR_SF] = R( 6, no_action,                 A),
        [ERPS_REQ_RAPS_SF]        = R( 7, unblock_non_failed_stop_tx, B),
        [ERPS_REQ_RAPS_MS]        = R( 8, unblock_non_failed_stop_tx, C),
        [ERPS_REQ_MS]             = R( 9, ms_switch,                 C),
        [ERPS_REQ_WTR_EXPIRES]    = R(10, no_action,                 A),
        [ERPS_REQ_WTR_RUNNING]    = R(11, no_action,                 A),
        [ERPS_REQ_WTB_EXPIRES]    = R(12, no_action,                 A),
        [ERPS_REQ_WTB_RUNNING]    = R(13, no_action,                 A),
        [ERPS_REQ_RAPS_NR_RB]     = R(14, raps_nr_rb_idle,           A),
        [ERPS_REQ_RAPS_NR]        = R(15, raps_nr_idle,              A),
    },
    [ERPS_STATE_PROTECTION] = {
        [ERPS_REQ_CLEAR]          = R(16, no_action,                 B),
        [ERPS_REQ_FS]             = R(17, fs_switch,                 D),
        [ERPS_REQ_RAPS_FS]        = R(18, raps_fs,                   D),
        [ERPS_REQ_LOCAL_SF]       = R(19, local_sf,                  B),
        [ERPS_REQ_LOCAL_CLEAR_SF] = R(20, clear_sf_protection,       E),
        [ERPS_REQ_RAPS_SF]        = R(21, no_action,                 B),
        [ERPS_REQ_RAPS_MS]        = R(22, no_action,                 B),
        [ERPS_REQ_MS]             = R(23, no_action,                 B),
        [ERPS_REQ_WTR_EXPIRES]    = R(24, no_action,                 B),
        [ERPS_REQ_WTR_RUNNING]    = R(25, no_action,                 B),
        [ERPS_REQ_WTB_EXPIRES]    = R(26, no_action,                 B),
        [ERPS_REQ_WTB_RUNNING]    = R(27, no_action,                 B),
        [ERPS_REQ_RAPS_NR_RB]     = R(28, no_action,                 E),
        [ERPS_REQ_RAPS_NR]        = R(29, owner_start_wtr,           E),
    },
    [ERPS_STATE_MANUAL_SWITCH] = {
        [ERPS_REQ_CLEAR]          = R(30, clear_command,             E),
        [ERPS_REQ_FS]             = R(31, fs_switch,                 D),
        [ERPS_REQ_RAPS_FS]        = R(32, raps_fs,                   D),
        [ERPS_REQ_LOCAL_SF]       = R(33, local_sf,                  B),
        [ERPS_REQ_LOCAL_CLEAR_SF] = R(34, no_action,                 C),
        [ERPS_REQ_RAPS_SF]        = R(35, unblock_non_failed_stop_tx, B),
        [ERPS_REQ_RAPS_MS]        = R(36, raps_ms_in_ms,             E),
        [ERPS_REQ_MS]             = R(37, no_action,                 C),
        [ERPS_REQ_WTR_EXPIRES]    = R(38, no_action,                 C),
        [ERPS_REQ_WTR_RUNNING]    = R(39, no_action,                 C),
        [ERPS_REQ_WTB_EXPIRES]    = R(40, no_action,                 C),
        [ERPS_REQ_WTB_RUNNING]    = R(41, no_action,                 C),
        [ERPS_REQ_RAPS_NR_RB]     = R(42, no_action,                 E),
        [ERPS_REQ_RAPS_NR]        = R(43, owner_start_wtb,           E),
    },
    [ERPS_STATE_FORCED_SWITCH] = {
        [ERPS_REQ_CLEAR]          = R(44, clear_command,             E),
        [ERPS_REQ_FS]             = R(45, fs_in_fs,                  D),
        [ERPS_REQ_RAPS_FS]        = R(46, no_action,                 D),
        [ERPS_REQ_LOCAL_SF]       = R(47, no_action,                 D),
        [ERPS_REQ_LOCAL_CLEAR_SF] = R(48, no_action,                 D),
        [ERPS_REQ_RAPS_SF]        = R(49, no_action,                 D),
        [ERPS_REQ_RAPS_MS]        = R(50, no_action,                 D),
        [ERPS_REQ_MS]             = R(51, no_action,                 D),
        [ERPS_REQ_WTR_EXPIRES]    = R(52, no_action,                 D),
        [ERPS_REQ_WTR_RUNNING]    = R(53, no_action,                 D),
        [ERPS_REQ_WTB_EXPIRES]    = R(54, no_action,                 D),
        [ERPS_REQ_WTB_RUNNING]    = R(55, no_action,                 D),
        [ERPS_REQ_RAPS_NR_RB]     = R(56, no_action,                 E),
        [ERPS_REQ_RAPS_NR]        = R(57, owner_start_wtb,           E),
    },
    [ERPS_STATE_PENDING] = {
        [ERPS_REQ_CLEAR]          = R(58, clear_pending,             A),
        [ERPS_REQ_FS]             = R(59, fs_switch_from_pending,    D),
        [ERPS_REQ_RAPS_FS]        = R(60, raps_fs_from_pending,      D),
        [ERPS_REQ_LOCAL_SF]       = R(61, local_sf_from_pending,     B),
        [ERPS_REQ_LOCAL_CLEAR_SF] = R(62, no_action,                 E),
        [ERPS_REQ_RAPS_SF]        = R(63, unblock_non_failed_stop_tx_from_pending, B),
        [ERPS_REQ_RAPS_MS]        = R(64, unblock_non_failed_stop_tx_from_pending, C),
        [ERPS_REQ_MS]             = R(65, ms_switch_from_pending,    C),
        [ERPS_REQ_WTR_EXPIRES]    = R(66, wtr_expires,               A),
        [ERPS_REQ_WTR_RUNNING]    = R(67, no_action,                 E),
        [ERPS_REQ_WTB_EXPIRES]    = R(68, wtb_expires,               A),
        [ERPS_REQ_WTB_RUNNING]    = R(69, no_action,                 E),
        [ERPS_REQ_RAPS_NR_RB]     = R(70, raps_nr_rb_pending,        A),
        [ERPS_REQ_RAPS_NR]        = R(71, raps_nr_pending,           E),
    },
};

#undef R
#undef A
#undef B
#undef C
#undef D
#undef E

static const struct fsm_row *row_for(erps_state_t s, erps_req_type_t t)
{
    if (s <= ERPS_STATE_INIT || s >= ERPS_STATE_COUNT || t <= ERPS_REQ_NONE || t >= ERPS_REQ_INIT)
        return NULL;
    const struct fsm_row *row = &fsm_table[s][t];
    return row->fn ? row : NULL;
}

uint8_t erps__fsm_row(erps_state_t s, erps_req_type_t t)
{
    const struct fsm_row *row = row_for(s, t);
    return row ? row->row : 0;
}

bool erps__fsm_row_is_noop(erps_state_t s, erps_req_type_t t)
{
    const struct fsm_row *row = row_for(s, t);
    return !row || (row->fn == no_action && row->next == s);
}

/* ------------------------------------------------------------------------------------------ */
/* Execution                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static void mark_row(erps_t *e, uint8_t row)
{
    if (row >= 1 && row <= ERPS_ROWS)
        e->rows_hit[row / 8] = (uint8_t)(e->rows_hit[row / 8] | (1u << (row % 8)));
}

static void set_state(erps_t *e, erps_state_t to, uint8_t row, erps_req_type_t req)
{
    erps_state_t from = e->state;
    if (from == to)
        return;
    e->state = to;
    e->state_changed = true;
    e->last_change = e->now;
    e->ctr.state_changes++;

    /* §10.1.9: a local command overridden by a higher top-priority request is forgotten. The
     * latch lives exactly as long as the node stays in the state the command created. */
    if (from == ERPS_STATE_MANUAL_SWITCH)
        e->ms_active = false;
    if (from == ERPS_STATE_FORCED_SWITCH)
        e->fs_active = false;

    erps__log(e, ERPS_LOG_INFO, "state %s -> %s (row %u, %s)", erps_state_str(from),
              erps_state_str(to), row, erps_req_type_str(req));
    erps_event_t ev = { .type = ERPS_EV_STATE_CHANGE };
    ev.u.state.from = from;
    ev.u.state.to = to;
    ev.u.state.row = row;
    ev.u.state.req = req;
    erps__notify(e, &ev);
}

void erps__fsm_init(erps_t *e)
{
    erps_req_t req = { .type = ERPS_REQ_INIT, .port = ERPS_PORT_NONE };
    erps__trace_begin(e, &req, 1);

    /* Row 1 */
    stop_guard(e);
    stop_wtr(e);
    stop_wtb(e);
    if (erps__is_owner(e)) {
        block_rpl(e);
        unblock_non_rpl(e);
        tx(e, ERPS_RAPS_NR, false, false, e->cfg.rpl_port);
        if (erps__revertive(e))
            start_wtr(e);
    } else if (erps__is_neighbour(e)) {
        block_rpl(e);
        unblock_non_rpl(e);
        tx(e, ERPS_RAPS_NR, false, false, e->cfg.rpl_port);
    } else {
        block_one(e);
        unblock_other(e);
        tx(e, ERPS_RAPS_NR, false, false, ERPS_PORT0);
    }

    e->state = ERPS_STATE_INIT;
    set_state(e, ERPS_STATE_PENDING, 1, ERPS_REQ_INIT);
    e->top = req;
    e->last_row = 1;
    mark_row(e, 1);
    e->ctr.fsm_runs++;
    erps__trace_end(e);
}

void erps__fsm_run(erps_t *e, const erps_req_t *req)
{
    const struct fsm_row *row = row_for(e->state, req->type);
    if (!row) {
        /* Only reachable through a bug: every (state, request) pair of Table 10-2 is defined. */
        erps__log(e, ERPS_LOG_ERR, "no Table 10-2 row for state %s request %s",
                  erps_state_str(e->state), erps_req_type_str(req->type));
        return;
    }

    erps__trace_begin(e, req, row->row);
    erps_state_t next = row->fn(e, req, row->next);
    set_state(e, next, row->row, req->type);
    e->top = *req;
    e->last_row = row->row;
    mark_row(e, row->row);
    e->ctr.fsm_runs++;
    erps__trace_end(e);
}

#ifdef ERPS_TESTING
void erps__test_set_state(erps_t *e, erps_state_t s)
{
    e->state = s;
}
#endif
