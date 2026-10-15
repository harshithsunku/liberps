/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Priority logic, local priority logic, timers, transmission, flush logic, validity check,
 * FOP, and API robustness — exercised through the public API with the mock platform.
 */
#include "mock.h"

#include <stdio.h>

/* Owner on port 1 that has reached Idle (WTR expired after start). */
static struct mock *idle_owner(void)
{
    struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT1);
    mock_advance(m, ERPS_SEC(301));
    REQUIRE(m->e->state == ERPS_STATE_IDLE);
    mock_reset(m);
    return m;
}

static struct mock *idle_node(void)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    REQUIRE(rx(m, ERPS_PORT1, ERPS_RAPS_NR, true, false, 1, 0x70) == ERPS_OK);
    REQUIRE(m->e->state == ERPS_STATE_IDLE);
    mock_reset(m);
    return m;
}

/* ---- priority logic -------------------------------------------------------------------- */

TEST(prio_local_sf_beats_raps_and_is_not_rerun)
{
    struct mock *m = idle_node();
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    CHECK_EQ(m->e->state, ERPS_STATE_PROTECTION);
    CHECK_EQ(m->e->last_row, 5);
    CHECK_TX(m, ERPS_RAPS_SF, false, false, 0);
    unsigned ntx = m->ntx;
    /* R-APS(SF) from the far end: outranked by local SF, no state machine run (no SF,DNF) */
    rx(m, ERPS_PORT1, ERPS_RAPS_SF, false, false, 1, 0x30);
    CHECK_EQ(m->e->last_row, 5);
    CHECK_EQ(m->ntx, ntx);
    CHECK_EQ(m->e->ctr.rx_suppressed, 1);
    CHECK(!m->e->tx.msg.dnf);
    mock_free(m);
}

TEST(prio_raps_fs_beats_local_sf)
{
    struct mock *m = idle_node();
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    rx(m, ERPS_PORT1, ERPS_RAPS_FS, false, false, 1, 0x30);
    CHECK_EQ(m->e->last_row, 18);
    CHECK_EQ(m->e->state, ERPS_STATE_FORCED_SWITCH);
    CHECK_PORTS(m, false, false); /* row 18 unblocks ring ports */
    mock_free(m);
}

TEST(prio_sf_ignored_in_fs_then_acted_on_after_clear)
{
    struct mock *m = idle_node();
    CHECK_EQ(erps_command(m->e, ERPS_CMD_FORCED_SWITCH, ERPS_PORT1, m->now, NULL), ERPS_OK);
    CHECK_EQ(m->e->state, ERPS_STATE_FORCED_SWITCH);
    CHECK_PORTS(m, false, true);
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    CHECK_EQ(m->e->state, ERPS_STATE_FORCED_SWITCH); /* note a) of Table 10-1 */
    CHECK_PORTS(m, false, true);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_CLEAR, ERPS_PORT0, m->now, NULL), ERPS_OK);
    /* row 44 -> Pending, then the pending SF is the top request: row 61 -> Protection */
    CHECK_EQ(m->e->state, ERPS_STATE_PROTECTION);
    CHECK_EQ(m->e->last_row, 61);
    CHECK_PORTS(m, true, false);
    CHECK_TX(m, ERPS_RAPS_SF, false, false, 0);
    mock_free(m);
}

TEST(prio_clear_sf_with_sf_on_other_port)
{
    struct mock *m = idle_node();
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    erps_set_signal_fail(m->e, ERPS_PORT1, true, m->now);
    CHECK_PORTS(m, true, true);
    CHECK_EQ(m->e->last_row, 19);
    /* port 0 recovers while port 1 is still failed: top stays local SF (port 1), row 19 */
    erps_set_signal_fail(m->e, ERPS_PORT0, false, m->now);
    CHECK_EQ(m->e->state, ERPS_STATE_PROTECTION);
    CHECK_EQ(m->e->last_row, 19);
    CHECK_PORTS(m, false, true);
    CHECK_TX(m, ERPS_RAPS_SF, false, true, 1);
    /* port 1 recovers: row 20 */
    erps_set_signal_fail(m->e, ERPS_PORT1, false, m->now);
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING);
    CHECK_EQ(m->e->last_row, 20);
    CHECK_PORTS(m, false, true);
    mock_free(m);
}

TEST(prio_own_node_id_ignored)
{
    struct mock *m = idle_node();
    uint8_t f[64];
    size_t n = mk_frame(f, sizeof f, ERPS_RAPS_SF, false, false, 0, 0x50); /* our ID */
    CHECK_EQ(erps_rx_frame(m->e, ERPS_PORT0, f, n, m->now), ERPS_OK);
    CHECK_EQ(m->e->state, ERPS_STATE_IDLE);
    CHECK_EQ(m->e->ctr.rx_own, 1);
    mock_free(m);
}

TEST(prio_wtr_running_suppresses_nr_at_owner)
{
    struct mock *m = idle_owner();
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    erps_set_signal_fail(m->e, ERPS_PORT0, false, m->now); /* row 20, WTR started */
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING);
    CHECK(m->e->wtr_deadline != ERPS_TIME_NEVER);
    mock_advance(m, ERPS_SEC(1)); /* past the guard */
    unsigned row = m->e->last_row;
    rx(m, ERPS_PORT0, ERPS_RAPS_NR, false, false, 0, 0x60); /* higher node ID */
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING);
    CHECK(m->e->last_row == row || m->e->last_row == 67);
    /* row 5 opened the RPL; row 71 (unblock non-failed) must NOT run at the owner while WTR
     * runs, so the recovered port 0 stays blocked */
    CHECK_PORTS(m, true, false);
    mock_advance(m, ERPS_SEC(300));
    CHECK_EQ(m->e->state, ERPS_STATE_IDLE);
    CHECK_EQ(m->e->last_row, 66);
    CHECK_PORTS(m, false, true);
    mock_free(m);
}

/* ---- local priority logic (§10.1.9) --------------------------------------------------- */

TEST(cmd_clear_validity)
{
    struct mock *m = idle_node();
    erps_reject_t why;
    CHECK_EQ(erps_command(m->e, ERPS_CMD_CLEAR, ERPS_PORT0, m->now, &why), ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_CLEAR_NOT_APPLICABLE);
    CHECK_EQ(m->nev, 1);
    CHECK_EQ(m->ev[0].type, ERPS_EV_CMD_REJECTED);
    mock_free(m);

    m = idle_owner();
    CHECK_EQ(erps_command(m->e, ERPS_CMD_CLEAR, ERPS_PORT0, m->now, &why), ERPS_OK);
    CHECK_EQ(m->e->last_row, 2);
    /* owner in FS state due to a remote FS: Clear not applicable */
    rx(m, ERPS_PORT0, ERPS_RAPS_FS, false, false, 0, 0x60);
    CHECK_EQ(m->e->state, ERPS_STATE_FORCED_SWITCH);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_CLEAR, ERPS_PORT0, m->now, &why), ERPS_E_REJECTED);
    mock_free(m);
}

TEST(cmd_ms_rules)
{
    struct mock *m = idle_node();
    erps_reject_t why;
    /* MS while an R-APS(MS) exists (state C): rejected */
    rx(m, ERPS_PORT1, ERPS_RAPS_MS, false, false, 1, 0x60);
    CHECK_EQ(m->e->state, ERPS_STATE_MANUAL_SWITCH);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT0, m->now, &why), ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_MS_IN_RING);
    CHECK(!m->e->ms_active);
    mock_free(m);

    /* MS with a local SF present: rejected (higher priority) */
    m = idle_node();
    erps_set_signal_fail(m->e, ERPS_PORT1, true, m->now);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT0, m->now, &why), ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_HIGHER_PRIORITY);
    mock_free(m);

    /* MS accepted from Idle, latched; R-APS(MS) from another node clears it (rule 2) */
    m = idle_node();
    CHECK_EQ(erps_command(m->e, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT0, m->now, &why), ERPS_OK);
    CHECK_EQ(m->e->state, ERPS_STATE_MANUAL_SWITCH);
    CHECK(m->e->ms_active);
    CHECK_PORTS(m, true, false);
    rx(m, ERPS_PORT1, ERPS_RAPS_MS, false, false, 1, 0x60);
    CHECK_EQ(m->e->last_row, 36);
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING);
    CHECK(!m->e->ms_active);
    CHECK_PORTS(m, true, false); /* keeps the port blocked */
    CHECK_TX(m, ERPS_RAPS_NR, false, false, 0);
    mock_free(m);
}

TEST(cmd_fs_rules_and_compat_v1)
{
    struct mock *m = idle_node();
    erps_reject_t why;
    CHECK_EQ(erps_command(m->e, ERPS_CMD_FORCED_SWITCH, ERPS_PORT0, m->now, &why), ERPS_OK);
    CHECK(m->e->fs_active);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_FORCED_SWITCH, ERPS_PORT1, m->now, &why), ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_FS_ACTIVE);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_FORCED_SWITCH, ERPS_PORT_NONE, m->now, &why), ERPS_E_INVAL);
    mock_free(m);

    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
    c.compat_version = 1;
    c.revertive = false;
    m = mock_new_cfg(&c);
    erps_start(m->e, m->now);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_FORCED_SWITCH, ERPS_PORT1, m->now, &why), ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_COMPAT_V1);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT1, m->now, &why), ERPS_E_REJECTED);
    CHECK(m->e->wtr_deadline != ERPS_TIME_NEVER); /* revertive forced (§10.1.13 a2) */
    CHECK_TX(m, ERPS_RAPS_NR, false, false, 0);
    CHECK_EQ(m->tx[0].pdu.version, 0); /* G.8032v1 PDU version */
    mock_free(m);
}

/* ---- timers ------------------------------------------------------------------------------ */

TEST(timer_holdoff)
{
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.holdoff_ms = 300;
    struct mock *m = mock_new_cfg(&c);
    erps_start(m->e, m->now);
    rx(m, ERPS_PORT1, ERPS_RAPS_NR, true, false, 1, 0x70);
    REQUIRE(m->e->state == ERPS_STATE_IDLE);
    /* transient defect shorter than the hold-off: nothing */
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    mock_advance(m, ERPS_MSEC(200));
    erps_set_signal_fail(m->e, ERPS_PORT0, false, m->now);
    mock_advance(m, ERPS_MSEC(500));
    CHECK_EQ(m->e->state, ERPS_STATE_IDLE);
    CHECK(!m->e->port[0].sf);
    /* persistent defect: SF after exactly 300 ms */
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    mock_advance(m, ERPS_MSEC(299));
    CHECK_EQ(m->e->state, ERPS_STATE_IDLE);
    mock_advance(m, ERPS_MSEC(1));
    CHECK_EQ(m->e->state, ERPS_STATE_PROTECTION);
    mock_free(m);
}

TEST(timer_guard_blocks_raps_but_not_events)
{
    struct mock *m = idle_node();
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    erps_set_signal_fail(m->e, ERPS_PORT0, false, m->now); /* row 20: guard 500 ms */
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING);
    mock_reset(m);
    rx(m, ERPS_PORT1, ERPS_RAPS_NR, true, false, 1, 0x70);
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING); /* discarded by the guard */
    CHECK_EQ(m->e->ctr.rx_guard, 1);
    uint8_t f[64];
    erps_frame_params_t fp = { .ring_id = 1, .vid = 100, .pcp = 7, .mel = 7, .version = 1 };
    erps_raps_msg_t ev = { .req = ERPS_RAPS_EVENT };
    ev.node_id[5] = 0x70;
    size_t n = erps_frame_encode(&fp, &ev, f, sizeof f);
    /* the flush logic saw the (NR,RB) despite the guard (Figure 10-1): new pair -> flush */
    CHECK_EQ(m->flushes, 1);
    erps_rx_frame(m->e, ERPS_PORT1, f, n, m->now);
    CHECK_EQ(m->flushes, 2); /* flush indication passes the guard */
    mock_advance(m, ERPS_MSEC(499));
    rx(m, ERPS_PORT1, ERPS_RAPS_NR, true, false, 1, 0x70);
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING);
    mock_advance(m, ERPS_MSEC(1));
    rx(m, ERPS_PORT1, ERPS_RAPS_NR, true, false, 1, 0x70);
    CHECK_EQ(m->e->state, ERPS_STATE_IDLE);
    mock_free(m);
}

TEST(timer_wtb_is_guard_plus_5s_and_clear_stops_without_expiry)
{
    struct mock *m = idle_owner();
    CHECK_EQ(erps_command(m->e, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT0, m->now, NULL), ERPS_OK);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_CLEAR, ERPS_PORT0, m->now, NULL), ERPS_OK);
    erps_trace_t h[4];
    size_t nh = erps_get_history(m->e, h, 4);
    REQUIRE(nh >= 2);
    CHECK_EQ(h[nh - 2].row, 30); /* Clear in MS */
    CHECK_EQ(h[nh - 1].row, 69); /* then WTB Running is the top request (no action) */
    CHECK(m->e->wtb_deadline == m->now + ERPS_MSEC(5500));
    /* Clear at the owner while WTB runs: row 58 reverts at once */
    mock_advance(m, ERPS_SEC(1));
    mock_reset(m);
    CHECK_EQ(erps_command(m->e, ERPS_CMD_CLEAR, ERPS_PORT0, m->now, NULL), ERPS_OK);
    CHECK_EQ(m->e->last_row, 58);
    CHECK_EQ(m->e->state, ERPS_STATE_IDLE);
    CHECK(m->e->wtb_deadline == ERPS_TIME_NEVER);
    mock_advance(m, ERPS_SEC(10));
    CHECK_EQ(m->e->last_row, 58); /* no WTB Expires after a stop */
    mock_free(m);
}

TEST(timer_wtr_start_does_not_restart)
{
    struct mock *m = idle_owner();
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    erps_set_signal_fail(m->e, ERPS_PORT0, false, m->now);
    erps_time_t d = m->e->wtr_deadline;
    mock_advance(m, ERPS_SEC(10));
    m->e->state = ERPS_STATE_PROTECTION;
    erps_req_t r = mk_raps(ERPS_REQ_RAPS_NR, ERPS_PORT1, 0x10);
    run_row(m, r); /* row 29: start WTR (already running) */
    CHECK_EQ(m->e->last_row, 29);
    CHECK(m->e->wtr_deadline == d);
    mock_free(m);
}

/* ---- transmission (§10.1.3) ----------------------------------------------------------------- */

TEST(tx_burst_then_periodic)
{
    struct mock *m = idle_node();
    erps_time_t t0 = m->now;
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    /* first message on both ports at t0 */
    REQUIRE(m->ntx == 2);
    CHECK_EQ(m->tx[0].t, t0);
    mock_advance(m, ERPS_SEC(11));
    /* t0, +3.333 ms, +6.666 ms, then +5 s, +10 s : 5 sends x 2 ports */
    CHECK_EQ(m->ntx, 10);
    CHECK_EQ(m->tx[2].t, t0 + 3333);
    CHECK_EQ(m->tx[4].t, t0 + 6666);
    CHECK_EQ(m->tx[6].t, t0 + 6666 + ERPS_SEC(5));
    CHECK_EQ(m->tx[8].t, t0 + 6666 + ERPS_SEC(10));
    for (unsigned i = 0; i < m->ntx; i++) {
        CHECK_EQ(m->tx[i].port, (erps_port_t)(i % 2));
        CHECK_EQ(m->tx[i].pdu.msg.req, ERPS_RAPS_SF);
        CHECK_EQ(m->tx[i].eth.vid, 100);
        CHECK_EQ(m->tx[i].eth.pcp, 7);
        CHECK_EQ(m->tx[i].pdu.mel, 7);
        CHECK_EQ(m->tx[i].pdu.version, 1);
        CHECK_EQ(m->tx[i].len, 60);
    }
    mock_free(m);
}

TEST(tx_event_burst_parallel)
{
    struct mock *m = idle_owner();
    REQUIRE(m->e->tx.active);
    CHECK_EQ(erps_send_flush_event(m->e, m->now), ERPS_OK);
    mock_advance(m, ERPS_MSEC(20));
    unsigned events = 0;
    for (unsigned i = 0; i < m->ntx; i++)
        if (m->tx[i].pdu.msg.req == ERPS_RAPS_EVENT)
            events++;
    CHECK_EQ(events, 6); /* 3 per port */
    CHECK(m->e->tx.active); /* continuous transmission untouched */
    mock_free(m);
}

TEST(tx_platform_failure_counted)
{
    struct mock *m = idle_node();
    m->fail_tx = 1;
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    CHECK_EQ(m->e->ctr.platform_errors, 1);
    mock_free(m);
}

/* ---- flush logic (§10.1.10) ---------------------------------------------------------------- */

TEST(flush_logic_pairs)
{
    struct mock *m = idle_node();
    /* SF (62,0) on port 1: new pair, differs from port 0 (empty) -> flush */
    rx(m, ERPS_PORT1, ERPS_RAPS_SF, false, false, 0, 62);
    CHECK_EQ(m->flushes, 1);
    /* repeat: same pair -> nothing */
    rx(m, ERPS_PORT1, ERPS_RAPS_SF, false, false, 0, 62);
    CHECK_EQ(m->flushes, 1);
    /* SF (89,1) on port 0: differs from both -> flush */
    rx(m, ERPS_PORT0, ERPS_RAPS_SF, false, false, 1, 89);
    CHECK_EQ(m->flushes, 2);
    /* the same (89,1) arriving now on port 1: equals port 0's pair -> stored, no flush */
    rx(m, ERPS_PORT1, ERPS_RAPS_SF, false, false, 1, 89);
    CHECK_EQ(m->flushes, 2);
    /* R-APS(NR) deletes the pair of its port without storing */
    rx(m, ERPS_PORT1, ERPS_RAPS_NR, false, false, 0, 62);
    CHECK(!m->e->port[1].pair_valid);
    CHECK_EQ(m->flushes, 2);
    /* DNF suppresses */
    rx(m, ERPS_PORT1, ERPS_RAPS_SF, false, true, 0, 0x33);
    CHECK_EQ(m->flushes, 2);
    CHECK(m->e->port[1].pair_valid);
    mock_free(m);
}

TEST(flush_logic_block_clears_pairs_and_flushes_coalesce)
{
    struct mock *m = idle_node();
    rx(m, ERPS_PORT1, ERPS_RAPS_SF, false, false, 0, 62);
    CHECK(m->e->port[1].pair_valid);
    m->flushes = 0;
    /* local SF: row 5 blocks port 0 (pairs cleared) and flushes: exactly one platform flush */
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    CHECK(!m->e->port[1].pair_valid);
    CHECK_EQ(m->flushes, 1);
    mock_free(m);
}

/* ---- validity check and Rx path -------------------------------------------------------- */

TEST(rx_validity_drops)
{
    struct mock *m = idle_node();
    uint8_t f[64];
    size_t n = mk_frame(f, sizeof f, ERPS_RAPS_SF, false, false, 0, 0x30);
    uint8_t g[64];

    memcpy(g, f, n);
    g[5] = 2; /* ring ID 2 */
    CHECK_EQ(erps_rx_frame(m->e, ERPS_PORT0, g, n, m->now), ERPS_E_DROPPED);
    CHECK_EQ(m->e->ctr.port[0].rx_drop[ERPS_DROP_RING_ID], 1);

    memcpy(g, f, n);
    g[15] = 101; /* VID 101 */
    erps_rx_frame(m->e, ERPS_PORT0, g, n, m->now);
    CHECK_EQ(m->e->ctr.port[0].rx_drop[ERPS_DROP_VID], 1);

    memcpy(g, f, n);
    g[18] = (uint8_t)((6u << 5) | 1u); /* MEL 6 */
    erps_rx_frame(m->e, ERPS_PORT0, g, n, m->now);
    CHECK_EQ(m->e->ctr.port[0].rx_drop[ERPS_DROP_MEL], 1);

    memcpy(g, f, n);
    g[19] = 1; /* CCM opcode */
    erps_rx_frame(m->e, ERPS_PORT0, g, n, m->now);
    CHECK_EQ(m->e->ctr.port[0].rx_drop[ERPS_DROP_OPCODE], 1);

    memcpy(g, f, n);
    g[22] = 0x30; /* reserved request 0011 */
    erps_rx_frame(m->e, ERPS_PORT0, g, n, m->now);
    CHECK_EQ(m->e->ctr.port[0].rx_drop[ERPS_DROP_REQ_RESERVED], 1);

    memcpy(g, f, n);
    g[0] = 0x01; g[1] = 0x80; g[2] = 0xC2; /* not the R-APS DA */
    erps_rx_frame(m->e, ERPS_PORT0, g, n, m->now);
    CHECK_EQ(m->e->ctr.port[0].rx_drop[ERPS_DROP_DA], 1);

    CHECK_EQ(erps_rx_frame(m->e, ERPS_PORT0, f, 20, m->now), ERPS_E_DROPPED);
    CHECK_EQ(m->e->ctr.port[0].rx_drop[ERPS_DROP_SHORT], 1);

    CHECK_EQ(m->e->state, ERPS_STATE_IDLE); /* none of these reached the state machine */
    CHECK_EQ(erps_rx_frame(m->e, ERPS_PORT0, f, n, m->now), ERPS_OK);
    CHECK_EQ(m->e->state, ERPS_STATE_PROTECTION);
    CHECK_EQ(m->e->ctr.port[0].rx_raps[ERPS_MSG_SF], 1);
    mock_free(m);
}

TEST(rx_pdu_api_and_sw_forwarding)
{
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.sw_raps_forwarding = true;
    struct mock *m = mock_new_cfg(&c);
    erps_start(m->e, m->now); /* port 0 blocked at init */
    uint8_t f[64];
    size_t n = mk_frame(f, sizeof f, ERPS_RAPS_NR, true, false, 1, 0x70);
    mock_reset(m);
    /* port 0 blocked: no forwarding */
    erps_rx_frame(m->e, ERPS_PORT1, f, n, m->now);
    CHECK_EQ(m->e->state, ERPS_STATE_IDLE);
    CHECK_PORTS(m, false, false);
    CHECK_EQ(m->ntx, 0);
    CHECK_EQ(m->e->ctr.port[1].sw_forwarded, 0);
    /* both unblocked now: forwarded unchanged to the other port */
    erps_rx_frame(m->e, ERPS_PORT1, f, n, m->now);
    REQUIRE(m->ntx == 1);
    CHECK_EQ(m->tx[0].port, ERPS_PORT0);
    CHECK_MEMEQ(m->tx[0].frame, f, n);
    /* own frames are never forwarded */
    size_t k = mk_frame(f, sizeof f, ERPS_RAPS_NR, true, false, 1, 0x50);
    erps_rx_frame(m->e, ERPS_PORT1, f, k, m->now);
    CHECK_EQ(m->ntx, 1);
    /* erps_rx_pdu path rebuilds the frame */
    erps_rx_meta_t meta = { .vid = 100, .pcp = 7 };
    erps_raps_dmac(1, meta.dst);
    meta.src[0] = 0x02;
    n = mk_frame(f, sizeof f, ERPS_RAPS_NR, true, false, 1, 0x70);
    CHECK_EQ(erps_rx_pdu(m->e, ERPS_PORT0, &meta, f + 18, n - 18, m->now), ERPS_OK);
    REQUIRE(m->ntx == 2);
    CHECK_EQ(m->tx[1].port, ERPS_PORT1);
    CHECK_EQ(m->tx[1].pdu.msg.rb, 1);
    meta.vid = 5;
    CHECK_EQ(erps_rx_pdu(m->e, ERPS_PORT0, &meta, f + 18, n - 18, m->now), ERPS_E_DROPPED);
    mock_free(m);
}

/* ---- FOP (§10.4) -------------------------------------------------------------------------- */

TEST(fop_pm_raise_and_clear)
{
    struct mock *m = idle_owner();
    rx(m, ERPS_PORT0, ERPS_RAPS_NR, true, false, 0, 0x60); /* another owner */
    CHECK(m->e->fop_pm);
    CHECK_EQ(m->e->ctr.fop_pm, 1);
    CHECK_EQ(m->nev >= 1, 1);
    mock_advance(m, ERPS_MSEC(17499));
    CHECK(m->e->fop_pm);
    mock_advance(m, ERPS_MSEC(2));
    CHECK(!m->e->fop_pm);
    mock_free(m);
}

TEST(fop_to_watchdog)
{
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.fop_to_enable = true;
    struct mock *m = mock_new_cfg(&c);
    erps_start(m->e, m->now);
    mock_advance(m, ERPS_SEC(10));
    rx(m, ERPS_PORT1, ERPS_RAPS_NR, true, false, 1, 0x70);
    mock_advance(m, ERPS_SEC(10));
    CHECK(m->e->port[0].fop_to);  /* nothing on port 0 for 20 s */
    CHECK(!m->e->port[1].fop_to); /* port 1 heard 10 s ago */
    mock_advance(m, ERPS_SEC(10));
    CHECK(m->e->port[1].fop_to);
    rx(m, ERPS_PORT1, ERPS_RAPS_NR, true, false, 1, 0x70);
    CHECK(!m->e->port[1].fop_to);
    /* SF on the port suppresses */
    erps_set_signal_fail(m->e, ERPS_PORT1, true, m->now);
    mock_advance(m, ERPS_SEC(40));
    CHECK(!m->e->port[1].fop_to);
    mock_free(m);
}

/* ---- API robustness ------------------------------------------------------------------------ */

TEST(api_config_validation)
{
    erps_config_t c;
    char why[160];
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_OK);
    c.ring_id = 240;
    CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_E_CONFIG);
    CHECK(strstr(why, "ring-id") != NULL);
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.wtr_ms = 90000; /* not a whole minute */
    CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_E_CONFIG);
    c.relaxed_timers = true;
    CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_OK);
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.guard_ms = 5;
    CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_E_CONFIG);
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.holdoff_ms = 150;
    CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_E_CONFIG);
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.node_id[0] = 0x01; /* multicast */
    CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_E_CONFIG);
    mock_cfg(&c, ERPS_ROLE_RPL_OWNER, ERPS_PORT_NONE);
    CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_E_CONFIG);
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.raps_vid = 0;
    CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_E_CONFIG);
    CHECK_EQ(erps_config_validate(NULL, NULL, 0), ERPS_E_INVAL);
}

TEST(api_null_and_invalid_args)
{
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    erps_t *e = NULL;
    CHECK_EQ(erps_create(NULL, &mock_ops, NULL, &e), ERPS_E_INVAL);
    CHECK_EQ(erps_create(&c, NULL, NULL, &e), ERPS_E_INVAL);
    erps_ops_t partial = mock_ops;
    partial.flush_fdb = NULL;
    CHECK_EQ(erps_create(&c, &partial, NULL, &e), ERPS_E_INVAL);
    CHECK_EQ(erps_start(NULL, 0), ERPS_E_INVAL);
    CHECK_EQ(erps_poll(NULL, 0), ERPS_E_INVAL);
    CHECK(erps_next_deadline(NULL) == ERPS_TIME_NEVER);
    erps_destroy(NULL);

    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    CHECK_EQ(erps_start(m->e, m->now), ERPS_E_STATE);
    CHECK_EQ(erps_set_signal_fail(m->e, ERPS_PORT_NONE, true, m->now), ERPS_E_INVAL);
    CHECK_EQ(erps_rx_frame(m->e, (erps_port_t)7, NULL, 0, m->now), ERPS_E_INVAL);
    CHECK_EQ(erps_command(m->e, (erps_cmd_t)9, ERPS_PORT0, m->now, NULL), ERPS_E_INVAL);
    CHECK_EQ(erps_rx_pdu(m->e, ERPS_PORT0, NULL, NULL, 0, m->now), ERPS_E_INVAL);
    mock_free(m);
}

TEST(api_static_init_and_size)
{
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    static unsigned char mem[1 << 16];
    REQUIRE(erps_instance_size() <= sizeof mem);
    erps_t *e = NULL;
    CHECK_EQ(erps_init_static(mem, 16, &c, &mock_ops, NULL, &e), ERPS_E_NOMEM);
    struct mock mk;
    memset(&mk, 0, sizeof mk);
    CHECK_EQ(erps_init_static(mem, sizeof mem, &c, &mock_ops, &mk, &e), ERPS_OK);
    mk.e = e;
    CHECK_EQ(erps_start(e, 0), ERPS_OK);
    erps_destroy(e); /* not heap: must not free */
}

TEST(api_reentrancy_and_clock)
{
    struct mock *m = idle_node();
    m->reenter = 1;
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now); /* tx() calls erps_poll() */
    CHECK_EQ(m->reenter_rc, ERPS_E_BUSY);
    erps_poll(m->e, m->now - 1000);
    CHECK_EQ(m->e->ctr.clock_backwards, 1);
    mock_free(m);
}

TEST(api_platform_port_retry)
{
    struct mock *m = idle_node();
    m->fail_set_port = 1;
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    CHECK(m->e->port[0].blocked);
    CHECK(!m->e->port[0].hw_synced);
    CHECK_EQ(m->e->ctr.platform_errors, 1);
    CHECK_EQ(m->port_mask[0], 0); /* platform still forwarding */
    mock_advance(m, ERPS_MSEC(100));
    CHECK(m->e->port[0].hw_synced);
    CHECK_EQ(m->port_mask[0], ERPS_BLOCK_TRAFFIC | ERPS_BLOCK_RAPS);
    mock_free(m);
}

TEST(api_stop_blocks_and_restart)
{
    struct mock *m = idle_node();
    CHECK_EQ(erps_stop(m->e, m->now, true), ERPS_OK);
    CHECK_PORTS(m, true, true);
    CHECK_EQ(m->e->state, ERPS_STATE_INIT);
    CHECK(erps_next_deadline(m->e) == ERPS_TIME_NEVER);
    CHECK_EQ(rx(m, ERPS_PORT0, ERPS_RAPS_SF, false, false, 0, 0x30), ERPS_E_DROPPED);
    CHECK_EQ(erps_start(m->e, m->now), ERPS_OK);
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING);
    mock_free(m);
}

TEST(api_dump_text_json_history)
{
    struct mock *m = idle_node();
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
    char small[16];
    int need = erps_dump(m->e, ERPS_DUMP_ALL, ERPS_FMT_TEXT, small, sizeof small);
    CHECK(need > (int)sizeof small);
    CHECK_EQ(strlen(small), sizeof small - 1);
    char *buf = malloc((size_t)need + 1);
    REQUIRE(buf);
    CHECK_EQ(erps_dump(m->e, ERPS_DUMP_ALL, ERPS_FMT_TEXT, buf, (size_t)need + 1), need);
    CHECK(strstr(buf, "Protection") != NULL);
    CHECK(strstr(buf, "row 5") != NULL);
    th_log("%s", buf);
    free(buf);

    need = erps_dump(m->e, ERPS_DUMP_ALL, ERPS_FMT_JSON, NULL, 0);
    buf = malloc((size_t)need + 1);
    REQUIRE(buf);
    erps_dump(m->e, ERPS_DUMP_ALL, ERPS_FMT_JSON, buf, (size_t)need + 1);
    int depth = 0, ok = 1;
    bool instr = false;
    for (char *p = buf; *p; p++) {
        if (*p == '"' && p[-1] != '\\')
            instr = !instr;
        if (instr)
            continue;
        if (*p == '{' || *p == '[')
            depth++;
        if (*p == '}' || *p == ']')
            depth--;
        if (depth < 0)
            ok = 0;
    }
    CHECK(ok && depth == 0 && !instr);
    CHECK(strstr(buf, "\"state\":\"Protection\"") != NULL);
    free(buf);

    erps_trace_t h[8];
    size_t n = erps_get_history(m->e, h, 8);
    REQUIRE(n >= 2);
    CHECK_EQ(h[n - 1].row, 5);
    CHECK_EQ(h[n - 1].from, ERPS_STATE_IDLE);
    CHECK_EQ(h[n - 1].to, ERPS_STATE_PROTECTION);
    CHECK(h[n - 2].seq < h[n - 1].seq);
    char line[256];
    erps_trace_format(&h[n - 1], line, sizeof line);
    CHECK(strstr(line, "A->B row 5") != NULL);
    mock_free(m);
}

TEST(api_history_wraps)
{
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.history_depth = 4;
    struct mock *m = mock_new_cfg(&c);
    erps_start(m->e, m->now);
    for (int i = 0; i < 10; i++) {
        erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);
        erps_set_signal_fail(m->e, ERPS_PORT0, false, m->now);
    }
    erps_trace_t h[16];
    size_t n = erps_get_history(m->e, h, 16);
    CHECK_EQ(n, 4);
    for (size_t i = 1; i < n; i++)
        CHECK_EQ(h[i].seq, h[i - 1].seq + 1);
    CHECK_EQ(h[n - 1].seq, m->e->seq);
    mock_free(m);
}

TEST(api_names_cover_all_values)
{
    for (int s = 0; s < ERPS_STATE_COUNT; s++)
        CHECK(strcmp(erps_state_str((erps_state_t)s), "?") != 0);
    for (int t = 0; t < ERPS_REQ_COUNT; t++)
        CHECK(strcmp(erps_req_type_str((erps_req_type_t)t), "?") != 0);
    for (int d = 0; d < ERPS_DROP_COUNT; d++)
        CHECK(strcmp(erps_drop_str((erps_drop_t)d), "?") != 0);
    for (int a = 0; a < ERPS_ACT_COUNT; a++)
        CHECK(strcmp(erps_action_str((erps_action_t)a), "?") != 0);
    for (int ev = 0; ev < ERPS_EV_COUNT; ev++)
        CHECK(strcmp(erps_event_str((erps_event_type_t)ev), "?") != 0);
    for (int err = -12; err <= 0; err++)
        CHECK(strcmp(erps_strerror((erps_err_t)err), "unknown error") != 0);
    CHECK_STREQ(erps_version(), "0.1.0");
}
