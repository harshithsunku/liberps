/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Mock platform for unit tests: records transmitted frames (decoded), port programming,
 * flushes and notifications. Tests include the internal header to set up preconditions
 * (port / timer state) directly before exercising one Table 10-2 row.
 */
#ifndef ERPS_TEST_MOCK_H
#define ERPS_TEST_MOCK_H

#include "core/erps_internal.h"
#include "erps/erps_pdu.h"
#include "harness.h"

#include <stdlib.h>
#include <string.h>

#define MOCK_MAX_TX 256
#define MOCK_MAX_EV 256

struct mock_tx {
    erps_port_t port;
    erps_time_t t;
    erps_pdu_t pdu;
    erps_eth_t eth;
    uint8_t frame[128];
    size_t len;
};

struct mock {
    erps_t *e;
    erps_time_t now;
    struct mock_tx tx[MOCK_MAX_TX];
    unsigned ntx;
    unsigned port_calls[2];
    unsigned port_mask[2];
    unsigned flushes;
    erps_event_t ev[MOCK_MAX_EV];
    unsigned nev;
    int fail_set_port; /* number of set_port_state calls to fail */
    int fail_tx;
    int reenter;       /* call erps_poll from inside tx() */
    int destroy_on_event; /* call erps_destroy from inside notify() */
    int fail_flush;
    erps_err_t reenter_rc;
    char last_log[512];
};

static int mock_tx_cb(void *user, erps_port_t port, const uint8_t *frame, size_t len)
{
    struct mock *m = user;
    if (m->reenter) {
        m->reenter_rc = erps_poll(m->e, m->now);
        m->reenter = 0;
    }
    if (m->fail_tx) {
        m->fail_tx--;
        return -1;
    }
    if (m->ntx < MOCK_MAX_TX) {
        struct mock_tx *t = &m->tx[m->ntx++];
        t->port = port;
        t->t = m->now;
        t->len = len < sizeof t->frame ? len : sizeof t->frame;
        memcpy(t->frame, frame, t->len);
        if (erps_eth_parse(frame, len, &t->eth) == ERPS_DROP_NONE)
            erps_pdu_decode(frame + t->eth.hdr_len, len - t->eth.hdr_len, &t->pdu);
    }
    return 0;
}

static int mock_port_cb(void *user, erps_port_t port, unsigned mask)
{
    struct mock *m = user;
    m->port_calls[port]++;
    if (m->fail_set_port) {
        m->fail_set_port--;
        return -5;
    }
    m->port_mask[port] = mask;
    return 0;
}

static int mock_flush_cb(void *user)
{
    struct mock *m = user;
    if (m->fail_flush) {
        m->fail_flush--;
        return -1;
    }
    m->flushes++;
    return 0;
}

static void mock_notify_cb(void *user, const erps_event_t *ev)
{
    struct mock *m = user;
    if (m->nev < MOCK_MAX_EV)
        m->ev[m->nev++] = *ev;
    if (m->destroy_on_event) {
        m->destroy_on_event = 0;
        erps_destroy(m->e);
    }
}

static void mock_log_cb(void *user, erps_log_level_t lvl, const char *msg)
{
    struct mock *m = user;
    (void)lvl;
    snprintf(m->last_log, sizeof m->last_log, "%s", msg);
    th_log("      log: %s\n", msg);
}

static const erps_ops_t mock_ops = {
    .tx = mock_tx_cb,
    .set_port_state = mock_port_cb,
    .flush_fdb = mock_flush_cb,
    .notify = mock_notify_cb,
    .log = mock_log_cb,
};

static const uint8_t MOCK_OWN_ID[6] = { 0x02, 0, 0, 0, 0, 0x50 };

static inline void mock_cfg(erps_config_t *c, erps_role_t role, erps_port_t rpl)
{
    erps_config_init(c);
    snprintf(c->name, sizeof c->name, "T");
    c->raps_vid = 100;
    memcpy(c->node_id, MOCK_OWN_ID, 6);
    c->role = role;
    c->rpl_port = rpl;
    c->log_level = ERPS_LOG_TRACE;
}

static inline struct mock *mock_new_cfg(const erps_config_t *c)
{
    struct mock *m = calloc(1, sizeof *m);
    REQUIRE(m != NULL);
    m->now = ERPS_SEC(1000);
    REQUIRE(erps_create(c, &mock_ops, m, &m->e) == ERPS_OK);
    return m;
}

static inline struct mock *mock_new(erps_role_t role, erps_port_t rpl)
{
    erps_config_t c;
    mock_cfg(&c, role, rpl);
    return mock_new_cfg(&c);
}

/* Create + start (row 1) and clear the recorders. */
static inline struct mock *mock_started(erps_role_t role, erps_port_t rpl)
{
    struct mock *m = mock_new(role, rpl);
    REQUIRE(erps_start(m->e, m->now) == ERPS_OK);
    return m;
}

static inline void mock_reset(struct mock *m)
{
    m->ntx = 0;
    m->flushes = 0;
    m->nev = 0;
    m->port_calls[0] = m->port_calls[1] = 0;
}

static inline void mock_free(struct mock *m)
{
    erps_destroy(m->e);
    free(m);
}

static inline void mock_advance(struct mock *m, erps_time_t dt)
{
    erps_time_t end = m->now + dt;
    for (;;) {
        erps_time_t d = erps_next_deadline(m->e);
        if (d > end)
            break;
        if (d > m->now)
            m->now = d;
        erps_poll(m->e, m->now);
    }
    m->now = end;
}

/* Force internal preconditions for a row test. */
static inline void mock_setup(struct mock *m, erps_state_t st, bool b0, bool b1)
{
    m->e->state = st;
    m->e->port[0].blocked = b0;
    m->e->port[1].blocked = b1;
    m->e->port[0].hw_synced = m->e->port[1].hw_synced = true;
    m->e->top.type = ERPS_REQ_NONE;
    m->e->tx.active = false; /* rows that Tx must produce a new frame */
    m->e->wtr_deadline = m->e->wtb_deadline = m->e->guard_deadline = ERPS_TIME_NEVER;
    mock_reset(m);
}

static inline erps_req_t mk_req(erps_req_type_t t, erps_port_t p)
{
    erps_req_t r;
    memset(&r, 0, sizeof r);
    r.type = t;
    r.port = p;
    return r;
}

static inline erps_req_t mk_raps(erps_req_type_t t, erps_port_t p, uint8_t id_last)
{
    erps_req_t r = mk_req(t, p);
    r.raps.req = t == ERPS_REQ_RAPS_SF   ? ERPS_RAPS_SF
                 : t == ERPS_REQ_RAPS_MS ? ERPS_RAPS_MS
                 : t == ERPS_REQ_RAPS_FS ? ERPS_RAPS_FS
                                         : ERPS_RAPS_NR;
    r.raps.rb = t == ERPS_REQ_RAPS_NR_RB;
    uint8_t id[6] = { 0x02, 0, 0, 0, 0, id_last };
    memcpy(r.raps.node_id, id, 6);
    return r;
}

static inline void run_row(struct mock *m, erps_req_t r)
{
    erps__test_fsm_run(m->e, &r, m->now);
}

/* Last transmitted R-APS (decoded); NULL if none. */
static inline const erps_raps_msg_t *last_tx(const struct mock *m)
{
    return m->ntx ? &m->tx[m->ntx - 1].pdu.msg : NULL;
}

/* Build a received R-APS frame for this instance's ring. */
static inline size_t mk_frame(uint8_t *buf, size_t len, uint8_t code, bool rb, bool dnf, uint8_t bpr,
                              uint8_t id_last)
{
    erps_frame_params_t fp = { .ring_id = 1, .vid = 100, .pcp = 7, .mel = 7, .version = 1 };
    fp.src[0] = 0x02;
    fp.src[5] = id_last;
    erps_raps_msg_t msg;
    memset(&msg, 0, sizeof msg);
    msg.req = code;
    msg.rb = rb;
    msg.dnf = dnf;
    msg.bpr = bpr;
    uint8_t id[6] = { 0x02, 0, 0, 0, 0, id_last };
    memcpy(msg.node_id, id, 6);
    return erps_frame_encode(&fp, &msg, buf, len);
}

static inline erps_err_t rx(struct mock *m, erps_port_t p, uint8_t code, bool rb, bool dnf,
                            uint8_t bpr, uint8_t id_last)
{
    uint8_t f[ERPS_FRAME_MAX_LEN];
    size_t n = mk_frame(f, sizeof f, code, rb, dnf, bpr, id_last);
    return erps_rx_frame(m->e, p, f, n, m->now);
}

#define CHECK_TX(m, code, rb_, dnf_, bpr_)                                                 \
    do {                                                                                   \
        const erps_raps_msg_t *tm_ = last_tx(m);                                           \
        REQUIRE(tm_ != NULL);                                                              \
        CHECK_EQ(tm_->req, (code));                                                        \
        CHECK_EQ(tm_->rb, (rb_));                                                          \
        CHECK_EQ(tm_->dnf, (dnf_));                                                        \
        CHECK_EQ(tm_->bpr, (bpr_));                                                        \
    } while (0)

#define CHECK_PORTS(m, b0, b1)                                                             \
    do {                                                                                   \
        CHECK_EQ((m)->e->port[0].blocked, (b0));                                           \
        CHECK_EQ((m)->e->port[1].blocked, (b1));                                           \
    } while (0)

#endif
