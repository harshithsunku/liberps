/* SPDX-License-Identifier: Apache-2.0 */
/* Regression tests for the findings of the first independent conformance audit (codex). */
#include "mock.h"

static struct mock *owner_after_sf_recovery(void)
{
    struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT1);
    mock_advance(m, ERPS_SEC(301));
    REQUIRE(m->e->state == ERPS_STATE_IDLE);
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now);  /* RPL opens */
    erps_set_signal_fail(m->e, ERPS_PORT0, false, m->now); /* WTR runs */
    REQUIRE(m->e->state == ERPS_STATE_PENDING);
    REQUIRE(m->e->wtr_deadline != ERPS_TIME_NEVER);
    mock_reset(m);
    return m;
}

/* 1: (NR,RB) must not be advertised while the RPL block is not in the dataplane */
TEST(audit1_failed_block_holds_advertisement)
{
    struct mock *m = owner_after_sf_recovery();
    m->fail_set_port = 1; /* the RPL block at WTR expiry fails once */
    mock_advance(m, ERPS_SEC(300));
    CHECK_EQ(m->e->last_row, 66);
    bool sent_rb = false;
    for (unsigned i = 0; i < m->ntx; i++)
        if (m->tx[i].pdu.msg.rb && m->tx[i].t < m->now)
            sent_rb = sent_rb || m->port_mask[1] == 0;
    CHECK(!sent_rb);
    /* after the retry succeeds the burst goes out, with the block in place */
    mock_advance(m, ERPS_MSEC(200));
    CHECK_EQ(m->port_mask[1], ERPS_BLOCK_TRAFFIC | ERPS_BLOCK_RAPS);
    REQUIRE(m->ntx >= 2);
    CHECK_TX(m, ERPS_RAPS_NR, true, false, 1);
    CHECK(!m->e->tx.held);
    mock_free(m);
}

TEST(audit1_stop_reports_failed_block)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    m->fail_set_port = 1;
    CHECK_EQ(erps_stop(m->e, m->now, true), ERPS_E_PLATFORM);
    mock_free(m);
}

/* 2: erps_destroy from a callback is deferred, no use-after-free (run under ASan) */
TEST(audit2_destroy_from_callback)
{
    struct mock *m = mock_new(ERPS_ROLE_NONE, ERPS_PORT0);
    m->destroy_on_event = 1;
    /* the first notify destroys the instance mid-row 1; it is released only when erps_start()
     * unwinds (ASan would flag any access to freed memory inside the library) */
    CHECK_EQ(erps_start(m->e, m->now), ERPS_OK);
    CHECK(m->nev >= 2); /* notifications after the destroy request still delivered safely */
    free(m);
}

/* 3: switching to non-revertive cancels the armed WTR */
TEST(audit3_set_nonrevertive_stops_wtr)
{
    struct mock *m = owner_after_sf_recovery();
    CHECK_EQ(erps_set_revertive(m->e, false), ERPS_OK);
    CHECK(m->e->wtr_deadline == ERPS_TIME_NEVER);
    mock_advance(m, ERPS_SEC(400));
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING); /* no automatic reversion */
    CHECK_EQ(erps_command(m->e, ERPS_CMD_CLEAR, ERPS_PORT0, m->now, NULL), ERPS_OK);
    CHECK_EQ(m->e->state, ERPS_STATE_IDLE);
    mock_free(m);
}

/* 4: a failed flush is retried */
TEST(audit4_flush_retry)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    rx(m, ERPS_PORT1, ERPS_RAPS_NR, true, false, 1, 0x70);
    mock_reset(m);
    m->fail_flush = 1;
    erps_set_signal_fail(m->e, ERPS_PORT0, true, m->now); /* row 5 flushes */
    CHECK_EQ(m->flushes, 0);
    CHECK_EQ(m->e->ctr.platform_errors, 1);
    mock_advance(m, ERPS_MSEC(150));
    CHECK_EQ(m->flushes, 1);
    mock_free(m);
}

/* 5: time origin near UINT64_MAX does not wrap deadlines into the past */
TEST(audit5_deadline_saturation)
{
    struct mock *m = mock_new(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
    m->now = ERPS_TIME_NEVER - 1000;
    CHECK_EQ(erps_start(m->e, m->now), ERPS_OK);
    CHECK(m->e->wtr_deadline > m->now);
    CHECK(m->e->tx.next > m->now);
    erps_poll(m->e, m->now);
    CHECK_EQ(m->e->state, ERPS_STATE_PENDING); /* WTR did not expire immediately */
    mock_free(m);
}

/* 7: Stop Tx also stops an event burst in progress */
TEST(audit7_stop_tx_stops_event_burst)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    CHECK_EQ(erps_send_flush_event(m->e, m->now), ERPS_OK);
    rx(m, ERPS_PORT1, ERPS_RAPS_NR, true, false, 1, 0x70); /* row 70: stop Tx */
    unsigned before = m->ntx;
    mock_advance(m, ERPS_MSEC(20));
    unsigned events = 0;
    for (unsigned i = before; i < m->ntx; i++)
        events += m->tx[i].pdu.msg.req == ERPS_RAPS_EVENT;
    CHECK_EQ(events, 0);
    mock_free(m);
}

/* 8: non-conforming Tx timing needs relaxed_timers */
TEST(audit8_tx_timing_validation)
{
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.tx_interval_ms = 1000;
    CHECK_EQ(erps_config_validate(&c, NULL, 0), ERPS_E_CONFIG);
    c.relaxed_timers = true;
    CHECK_EQ(erps_config_validate(&c, NULL, 0), ERPS_OK);
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.tx_burst_us = 5000;
    CHECK_EQ(erps_config_validate(&c, NULL, 0), ERPS_E_CONFIG);
}

/* 9: software R-APS_FF forwards transit R-APS independent of the local validity check */
TEST(audit9_sw_forwarding_like_hardware_ff)
{
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.sw_raps_forwarding = true;
    c.mel = 5;
    struct mock *m = mock_new_cfg(&c);
    erps_start(m->e, m->now);
    m->e->port[0].blocked = false; /* both ring ports forwarding */
    mock_reset(m);
    uint8_t f[64];
    size_t n = mk_frame(f, sizeof f, ERPS_RAPS_SF, false, false, 0, 0x30); /* MEL 7, ring 1 */
    CHECK_EQ(erps_rx_frame(m->e, ERPS_PORT1, f, n, m->now), ERPS_E_DROPPED); /* MEL != 5 */
    CHECK_EQ(m->ntx, 1); /* ...but forwarded (MEL 7 >= 5) */
    f[5] = 9; /* another ring ID on our R-APS VLAN */
    erps_rx_frame(m->e, ERPS_PORT1, f, n, m->now);
    CHECK_EQ(m->ntx, 2);
    f[5] = 1;
    f[18] = (uint8_t)((3u << 5) | 1u); /* MEL 3 < 5: MEG level filtering, not forwarded */
    erps_rx_frame(m->e, ERPS_PORT1, f, n, m->now);
    CHECK_EQ(m->ntx, 2);
    mock_free(m);
}

/* 11: the three copies of an Event burst flush once (10 ms indication) */
TEST(audit11_event_burst_flushes_once)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    uint8_t f[64];
    erps_frame_params_t fp = { .ring_id = 1, .vid = 100, .pcp = 7, .mel = 7, .version = 1 };
    erps_raps_msg_t ev = { .req = ERPS_RAPS_EVENT };
    ev.node_id[5] = 0x70;
    size_t n = erps_frame_encode(&fp, &ev, f, sizeof f);
    mock_reset(m);
    for (int i = 0; i < 3; i++) {
        erps_rx_frame(m->e, ERPS_PORT1, f, n, m->now);
        m->now += 3333;
    }
    CHECK_EQ(m->flushes, 1);
    m->now += ERPS_MSEC(20);
    erps_rx_frame(m->e, ERPS_PORT1, f, n, m->now);
    CHECK_EQ(m->flushes, 2);
    mock_free(m);
}

/* 13: restart resets receive diagnostics */
TEST(audit13_restart_resets_rx_diagnostics)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    rx(m, ERPS_PORT1, ERPS_RAPS_SF, false, false, 0, 0x70);
    erps_stop(m->e, m->now, true);
    erps_start(m->e, m->now);
    erps_status_t st;
    erps_get_status(m->e, &st);
    CHECK(!st.port[1].rx_seen);
    CHECK(!st.port[1].flush_pair_valid);
    mock_free(m);
}
