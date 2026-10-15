/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R-APS message transmission (§10.1.3).
 *
 * Continuous slot: "Tx R-APS(m)" with new information sends immediately, then two more copies
 * spaced by tx_burst_us (3.33 ms: fast protection survives the loss of two messages), then one
 * message every tx_interval_ms (5 s) until replaced or stopped. Repeating the same information
 * does not restart the burst.
 *
 * Event slot: a single burst of three messages, transmitted in parallel with (and independent
 * of) the continuous slot.
 *
 * Locally generated R-APS goes out on both ring ports whether or not they are blocked (§9.5 b).
 */
#include "erps_internal.h"

#include <string.h>

bool erps__msg_same(const erps_raps_msg_t *a, const erps_raps_msg_t *b)
{
    return a->req == b->req && a->subcode == b->subcode && a->rb == b->rb && a->dnf == b->dnf &&
           a->bpr == b->bpr && memcmp(a->node_id, b->node_id, 6) == 0;
}

static uint8_t tx_version(const erps_t *e)
{
    if (e->cfg.tx_version != ERPS_TX_VERSION_AUTO)
        return e->cfg.tx_version;
    return (uint8_t)(e->cfg.compat_version == 1 ? ERPS_VERSION_V1 : ERPS_VERSION_V2);
}

static void send_both(erps_t *e, const erps_raps_msg_t *m)
{
    erps_msg_kind_t kind = erps_msg_kind(m);
    for (int p = 0; p < 2; p++) {
        static const uint8_t zero[6];
        erps_frame_params_t fp;
        uint8_t frame[ERPS_FRAME_MAX_LEN];
        fp.ring_id = e->cfg.ring_id;
        memcpy(fp.src, memcmp(e->cfg.port[p].mac, zero, 6) ? e->cfg.port[p].mac : e->cfg.node_id,
               6);
        fp.vid = e->cfg.raps_vid;
        fp.pcp = e->cfg.raps_pcp;
        fp.mel = e->cfg.mel;
        fp.version = tx_version(e);
        size_t len = erps_frame_encode(&fp, m, frame, sizeof frame);
        int rc = e->ops.tx(e->user, (erps_port_t)p, frame, len);
        if (rc)
            erps__platform_error(e, "tx", rc, (erps_port_t)p);
        else if (kind < ERPS_MSG_COUNT)
            e->ctr.port[p].tx_raps[kind]++;
    }
}

static erps_time_t after_send(const erps_t *e, uint8_t sent)
{
    if (sent < 3)
        return erps__tadd(e->now, ERPS_USEC(e->cfg.tx_burst_us));
    return erps__tadd(e->now, ERPS_MSEC(e->cfg.tx_interval_ms));
}

/* First transmission of new information: the burst starts now, unless a port block the state
 * machine relies on is not yet in the dataplane (platform failure being retried). Peers act on
 * new information by unblocking, so it must never run ahead of our own blocks. */
static void start_burst(erps_t *e)
{
    if (!erps__blocks_confirmed(e)) {
        e->tx.held = true;
        e->tx.sent = 0;
        e->tx.next = ERPS_TIME_NEVER; /* released by erps__tx_poll() once blocks are confirmed */
        erps__log(e, ERPS_LOG_WARN, "tx held: waiting for the platform to confirm port blocks");
        return;
    }
    e->tx.held = false;
    send_both(e, &e->tx.msg);
    e->tx.sent = 1;
    e->tx.next = after_send(e, e->tx.sent);
}

void erps__tx_start(erps_t *e, const erps_raps_msg_t *m)
{
    if (e->tx.active && erps__msg_same(&e->tx.msg, m)) {
        erps__trace_action(e, ERPS_ACT_TX_SAME);
        return;
    }
    e->tx.active = true;
    e->tx.msg = *m;
    e->tx.msg.status = erps_status_octet(m->rb, m->dnf, m->bpr);
    erps__trace_action(e, ERPS_ACT_TX);
    erps__trace_tx(e, &e->tx.msg);
    if (e->ops.log && e->cfg.log_level >= ERPS_LOG_DEBUG) {
        char b[96];
        erps_raps_msg_format(&e->tx.msg, b, sizeof b);
        erps__log(e, ERPS_LOG_DEBUG, "tx %s", b);
    }
    start_burst(e);
}

/* Stop Tx R-APS: "stopping transmission of any R-APS messages" (§10.1.3), event bursts too. */
void erps__tx_stop(erps_t *e)
{
    e->ev.active = false;
    e->ev.next = ERPS_TIME_NEVER;
    if (!e->tx.active)
        return;
    e->tx.active = false;
    e->tx.held = false;
    e->tx.next = ERPS_TIME_NEVER;
    erps__trace_action(e, ERPS_ACT_STOP_TX);
    erps__log(e, ERPS_LOG_DEBUG, "tx stopped");
}

void erps__tx_event(erps_t *e)
{
    memset(&e->ev.msg, 0, sizeof e->ev.msg);
    e->ev.msg.req = ERPS_RAPS_EVENT; /* sub-code 0000 = flush request, status 0 */
    memcpy(e->ev.msg.node_id, e->cfg.node_id, 6);
    e->ev.active = true;
    e->ev.sent = 1;
    send_both(e, &e->ev.msg);
    e->ev.next = erps__tadd(e->now, ERPS_USEC(e->cfg.tx_burst_us));
}

void erps__tx_poll(erps_t *e)
{
    if (e->tx.active && e->tx.held) {
        if (erps__blocks_confirmed(e))
            start_burst(e);
    } else if (e->tx.active && e->now >= e->tx.next) {
        send_both(e, &e->tx.msg);
        if (e->tx.sent < 255)
            e->tx.sent++;
        e->tx.next = after_send(e, e->tx.sent);
    }
    if (e->ev.active && e->now >= e->ev.next) {
        send_both(e, &e->ev.msg);
        e->ev.sent++;
        if (e->ev.sent >= 3) {
            e->ev.active = false;
            e->ev.next = ERPS_TIME_NEVER;
        } else {
            e->ev.next = erps__tadd(e->now, ERPS_USEC(e->cfg.tx_burst_us));
        }
    }
}

erps_time_t erps__tx_next(const erps_t *e)
{
    erps_time_t t = ERPS_TIME_NEVER;
    if (e->tx.active)
        t = e->tx.next;
    if (e->ev.active && e->ev.next < t)
        t = e->ev.next;
    return t;
}

void erps__tx_reset(erps_t *e)
{
    memset(&e->tx, 0, sizeof e->tx);
    memset(&e->ev, 0, sizeof e->ev);
    e->tx.next = e->ev.next = ERPS_TIME_NEVER;
}
