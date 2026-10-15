/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Internal definitions shared by the core translation units. Not installed.
 *
 * Module map (one file per G.8032 sub-process, see Figure 10-1):
 *   erps.c      lifecycle, input plumbing, local defect logic (§10.1.7), hold-off (§10.1.8),
 *               validity-check wiring (§10.1.6), poll / deadlines
 *   priority.c  priority logic (§10.1.1), local priority logic (§10.1.9), compat (§10.1.13)
 *   fsm.c       R-APS request processing — Table 10-2 (§10.1.2)
 *   tx.c        R-APS message transmission (§10.1.3)
 *   flush.c     flush logic (§10.1.10) and coalesced FDB flush (§9.6)
 *   fop.c       failure of protocol defects (§10.4)
 *   trace.c     counters, transition history, dumps
 *   names.c     enum <-> string, formatting helpers
 *   config.c    defaults and validation (§10.1.4, §10.1.5, §10.1.8, §10.1.13)
 *   pdu.c       R-APS PDU codec (§10.3)
 */
#ifndef ERPS_INTERNAL_H
#define ERPS_INTERNAL_H

#include "erps/erps.h"
#include "erps/erps_pdu.h"

#include <stdarg.h>

#define ERPS_MAGIC 0x45525053u /* "ERPS" */
#define ERPS_ROWS 71           /* Table 10-2 */
#define ERPS_WTB_EXTRA_MS 5000u /* WTB = guard + 5 s (§10.1.4 b) */
#define ERPS_PLATFORM_RETRY_US ERPS_MSEC(100)

struct erps_port {
    bool blocked;          /* PortState as decided by the state machine */
    bool hw_synced;        /* the platform accepted the last set_port_state() */
    bool ssf;              /* raw ETH_CI_SSF */
    bool sf;               /* SF declared by local defect logic (after hold-off) */
    erps_time_t holdoff_deadline;

    /* flush logic (§10.1.10): last (node ID, BPR) received on this port */
    bool pair_valid;
    uint8_t pair_node[6];
    uint8_t pair_bpr;

    bool rx_seen;
    erps_time_t last_rx_time;
    erps_raps_msg_t last_rx;

    bool fop_to;
    erps_time_t fop_to_deadline;
};

/* A transmit slot: continuous R-APS (§10.1.3) or an event burst. */
struct erps_txslot {
    bool active;
    uint8_t sent;          /* messages sent since the information last changed */
    erps_raps_msg_t msg;
    erps_time_t next;
};

struct erps {
    uint32_t magic;
    erps_config_t cfg;
    erps_ops_t ops;
    void *user;
    bool heap;             /* allocated by erps_create() */

    bool running;
    erps_state_t state;
    erps_time_t now;       /* time of the input being processed */
    bool now_valid;
    int depth;             /* re-entrancy guard */

    struct erps_port port[2];

    /* latched local operator commands (§10.1.9) */
    bool fs_active;
    bool ms_active;
    erps_port_t cmd_port;

    /* timers; ERPS_TIME_NEVER when stopped */
    erps_time_t wtr_deadline;
    erps_time_t wtb_deadline;
    erps_time_t guard_deadline;
    erps_time_t retry_deadline;

    struct erps_txslot tx; /* continuous message */
    struct erps_txslot ev; /* event burst (parallel, §10.1.3) */

    /* priority logic bookkeeping */
    erps_req_t top;        /* last request signalled to the state machine */
    uint8_t last_row;
    erps_time_t last_change;
    bool state_changed;    /* set by a state machine run that changed state */

    bool fop_pm;
    erps_time_t fop_pm_deadline;

    bool flush_pending;    /* coalesced flush, committed when the input completes */

    /* debugging */
    erps_trace_t *cur;     /* history entry being filled by the running row */
    erps_trace_t hist[ERPS_HISTORY_MAX];
    uint32_t hist_next;    /* next slot to write */
    uint32_t hist_count;
    uint32_t seq;
    uint8_t rows_hit[9];
    erps_counters_t ctr;
};

/* ---- erps.c ---------------------------------------------------------------------------- */
void erps__log(erps_t *e, erps_log_level_t lvl, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void erps__notify(erps_t *e, erps_event_t *ev);
void erps__platform_error(erps_t *e, const char *op, int rc, erps_port_t port);
void erps__port_set(erps_t *e, erps_port_t p, bool blocked);
erps_time_t erps__deadline_after(const erps_t *e, uint64_t ms);

static inline erps_port_t erps__other(erps_port_t p)
{
    return p == ERPS_PORT0 ? ERPS_PORT1 : ERPS_PORT0;
}
static inline bool erps__is_owner(const erps_t *e)
{
    return e->cfg.role == ERPS_ROLE_RPL_OWNER;
}
static inline bool erps__is_neighbour(const erps_t *e)
{
    return e->cfg.role == ERPS_ROLE_RPL_NEIGHBOUR;
}
/* Revertive mode as seen by the state machine (compat v1 forces revertive, §10.1.13 a2). */
static inline bool erps__revertive(const erps_t *e)
{
    return e->cfg.compat_version == 1 ? true : e->cfg.revertive;
}
static inline uint8_t erps__ports_mask(const erps_t *e)
{
    return (uint8_t)((e->port[0].blocked ? 1u : 0u) | (e->port[1].blocked ? 2u : 0u));
}
static inline uint8_t erps__sf_mask(const erps_t *e)
{
    return (uint8_t)((e->port[0].sf ? 1u : 0u) | (e->port[1].sf ? 2u : 0u));
}

/* ---- priority.c ------------------------------------------------------------------------ */
void erps__prio_persistent(const erps_t *e, erps_req_t *out);
bool erps__req_same(const erps_req_t *a, const erps_req_t *b);
void erps__prio_event(erps_t *e, const erps_req_t *ev);
void erps__prio_raps(erps_t *e, const erps_req_t *ev);
erps_err_t erps__prio_command(erps_t *e, erps_cmd_t cmd, erps_port_t port, erps_reject_t *why);

/* ---- fsm.c ------------------------------------------------------------------------------ */
void erps__fsm_init(erps_t *e);                          /* row 1 */
void erps__fsm_run(erps_t *e, const erps_req_t *req);    /* rows 2..71 */
uint8_t erps__fsm_row(erps_state_t s, erps_req_type_t t);
bool erps__fsm_row_is_noop(erps_state_t s, erps_req_type_t t);

/* ---- tx.c ------------------------------------------------------------------------------- */
void erps__tx_start(erps_t *e, const erps_raps_msg_t *m);
void erps__tx_stop(erps_t *e);
void erps__tx_event(erps_t *e);
void erps__tx_poll(erps_t *e);
erps_time_t erps__tx_next(const erps_t *e);
void erps__tx_reset(erps_t *e);
bool erps__msg_same(const erps_raps_msg_t *a, const erps_raps_msg_t *b);

/* ---- flush.c ---------------------------------------------------------------------------- */
void erps__flush_rx(erps_t *e, erps_port_t p, const erps_raps_msg_t *m);
void erps__flush_clear_pairs(erps_t *e);
void erps__flush_request(erps_t *e);
void erps__flush_commit(erps_t *e);

/* ---- fop.c ------------------------------------------------------------------------------ */
void erps__fop_rx(erps_t *e, erps_port_t p, const erps_raps_msg_t *m);
void erps__fop_poll(erps_t *e);
erps_time_t erps__fop_next(const erps_t *e);
void erps__fop_reset(erps_t *e);

/* ---- trace.c ---------------------------------------------------------------------------- */
void erps__trace_begin(erps_t *e, const erps_req_t *req, uint8_t row);
void erps__trace_action(erps_t *e, erps_action_t a);
void erps__trace_tx(erps_t *e, const erps_raps_msg_t *m);
void erps__trace_end(erps_t *e);

/* ---- names.c ---------------------------------------------------------------------------- */
int erps__fmt_mac(const uint8_t mac[6], char *buf, size_t len);

/* ---- test hooks (tests only) ------------------------------------------------------------ */
#ifdef ERPS_TESTING
/* Run one Table 10-2 row for an arbitrary request, bypassing the priority logic. */
void erps__test_fsm_run(erps_t *e, const erps_req_t *req, erps_time_t now);
void erps__test_set_state(erps_t *e, erps_state_t s);
#endif

#endif /* ERPS_INTERNAL_H */
