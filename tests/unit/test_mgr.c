/* SPDX-License-Identifier: Apache-2.0 */
/* Instance manager (demux, fan-out, polling) and remaining configuration validation branches. */
#include "erps/erps_mgr.h"
#include "mock.h"

static void cfg_for(erps_config_t *c, const char *name, uint32_t h0, uint32_t h1, uint8_t ring,
                    uint16_t vid)
{
    mock_cfg(c, ERPS_ROLE_NONE, ERPS_PORT0);
    snprintf(c->name, sizeof c->name, "%s", name);
    c->port[0].handle = h0;
    c->port[1].handle = h1;
    c->ring_id = ring;
    c->raps_vid = vid;
}

TEST(mgr_add_find_remove)
{
    erps_mgr_t *m = erps_mgr_create();
    REQUIRE(m);
    struct mock mk1, mk2;
    memset(&mk1, 0, sizeof mk1);
    memset(&mk2, 0, sizeof mk2);
    erps_config_t c;
    erps_t *a = NULL, *b = NULL, *x = NULL;
    cfg_for(&c, "A", 10, 11, 1, 100);
    CHECK_EQ(erps_mgr_add(m, &c, &mock_ops, &mk1, &a), ERPS_OK);
    CHECK_EQ(erps_mgr_add(m, &c, &mock_ops, &mk1, &x), ERPS_E_EXISTS); /* same name */
    cfg_for(&c, "B", 11, 12, 1, 100);
    CHECK_EQ(erps_mgr_add(m, &c, &mock_ops, &mk2, &x), ERPS_E_EXISTS); /* same (ring, vid) on port 11 */
    cfg_for(&c, "B", 10, 11, 1, 101);
    CHECK_EQ(erps_mgr_add(m, &c, &mock_ops, &mk2, &b), ERPS_OK); /* other R-APS VID: fine */
    cfg_for(&c, "C", 5, 5, 1, 102);
    CHECK_EQ(erps_mgr_add(m, &c, &mock_ops, &mk2, &x), ERPS_E_CONFIG); /* same port twice */
    cfg_for(&c, "", 6, 7, 1, 103);
    CHECK_EQ(erps_mgr_add(m, &c, &mock_ops, &mk2, &x), ERPS_E_CONFIG); /* no name */
    CHECK_EQ(erps_mgr_count(m), 2);
    CHECK(erps_mgr_find(m, "B") == b);
    CHECK(erps_mgr_find(m, "Z") == NULL);
    CHECK(erps_mgr_at(m, 0) == a);
    CHECK(erps_mgr_at(m, 5) == NULL);
    CHECK_EQ(erps_mgr_remove(m, a), ERPS_OK);
    CHECK_EQ(erps_mgr_remove(m, a), ERPS_E_NOTFOUND);
    CHECK_EQ(erps_mgr_count(m), 1);
    CHECK(erps_mgr_at(m, 0) == b);
    erps_mgr_destroy(m);
    erps_mgr_destroy(NULL);
    CHECK_EQ(erps_mgr_add(NULL, &c, &mock_ops, NULL, &x), ERPS_E_INVAL);
    CHECK(erps_mgr_next_deadline(NULL) == ERPS_TIME_NEVER);
}

TEST(mgr_demux_signal_fail_and_poll)
{
    erps_mgr_t *m = erps_mgr_create();
    struct mock ma, mb;
    memset(&ma, 0, sizeof ma);
    memset(&mb, 0, sizeof mb);
    erps_config_t c;
    erps_t *a = NULL, *b = NULL;
    cfg_for(&c, "A", 10, 11, 1, 100);
    REQUIRE(erps_mgr_add(m, &c, &mock_ops, &ma, &a) == ERPS_OK);
    cfg_for(&c, "B", 10, 11, 2, 200); /* second instance on the same ports */
    REQUIRE(erps_mgr_add(m, &c, &mock_ops, &mb, &b) == ERPS_OK);
    ma.e = a;
    mb.e = b;
    erps_time_t now = ERPS_SEC(10);
    erps_start(a, now);
    erps_start(b, now);

    /* frame for ring 2 / VID 200 received on handle 11 -> instance B, port 1 */
    erps_frame_params_t fp = { .ring_id = 2, .vid = 200, .pcp = 7, .mel = 7, .version = 1 };
    erps_raps_msg_t msg = { .req = ERPS_RAPS_NR, .rb = true, .bpr = 1 };
    msg.node_id[0] = 2;
    msg.node_id[5] = 0x77;
    uint8_t f[64];
    size_t n = erps_frame_encode(&fp, &msg, f, sizeof f);
    CHECK_EQ(erps_mgr_rx_frame(m, 11, f, n, now), ERPS_OK);
    erps_status_t st;
    erps_get_status(b, &st);
    CHECK_EQ(st.state, ERPS_STATE_IDLE);
    CHECK(st.port[1].rx_seen);
    erps_get_status(a, &st);
    CHECK_EQ(st.state, ERPS_STATE_PENDING);
    /* unknown port, wrong VID, untagged/garbage */
    CHECK_EQ(erps_mgr_rx_frame(m, 99, f, n, now), ERPS_E_NOTFOUND);
    f[15] = 1;
    CHECK_EQ(erps_mgr_rx_frame(m, 11, f, n, now), ERPS_E_NOTFOUND);
    CHECK_EQ(erps_mgr_rx_frame(m, 11, f, 5, now), ERPS_E_NOTFOUND);
    CHECK_EQ(erps_mgr_rx_frame(m, 11, NULL, 5, now), ERPS_E_INVAL);

    /* SF on handle 10 reaches both instances */
    CHECK_EQ(erps_mgr_set_signal_fail(m, 10, true, now), ERPS_OK);
    erps_get_status(a, &st);
    CHECK(st.port[0].sf);
    erps_get_status(b, &st);
    CHECK(st.port[0].sf);
    CHECK_EQ(st.state, ERPS_STATE_PROTECTION);
    CHECK_EQ(erps_mgr_set_signal_fail(m, 42, true, now), ERPS_E_NOTFOUND);

    /* polling at the aggregated deadline advances both */
    erps_time_t d = erps_mgr_next_deadline(m);
    CHECK(d > now && d != ERPS_TIME_NEVER);
    CHECK(d <= erps_next_deadline(a) && d <= erps_next_deadline(b));
    unsigned before = ma.ntx + mb.ntx;
    CHECK_EQ(erps_mgr_poll(m, d), ERPS_OK);
    CHECK(ma.ntx + mb.ntx > before);
    erps_mgr_destroy(m);
}

TEST(config_validation_branches)
{
    erps_config_t c;
    char why[200];
#define BAD(stmt)                                                                            \
    do {                                                                                     \
        mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);                                            \
        stmt;                                                                                \
        CHECK_EQ(erps_config_validate(&c, why, sizeof why), ERPS_E_CONFIG);                  \
        th_log("      %s\n", why);                                                           \
    } while (0)
    BAD(memset(c.name, 'x', sizeof c.name));
    BAD(memset(c.port[1].name, 'x', sizeof c.port[1].name));
    BAD(c.ring_id = 0);
    BAD(c.mel = 8);
    BAD(memset(c.node_id, 0, 6));
    BAD(c.role = (erps_role_t)7);
    BAD(c.compat_version = 3);
    BAD((c.relaxed_timers = true, c.wtr_ms = 0));
    BAD((c.relaxed_timers = true, c.guard_ms = 0));
    BAD((c.relaxed_timers = true, c.holdoff_ms = 0xFFFFFFFFu));
    BAD(c.wtr_ms = 13 * 60000);
    BAD(c.guard_ms = 2010);
    BAD(c.holdoff_ms = 10100);
    BAD(c.raps_pcp = 8);
    BAD(c.tx_version = 32);
    BAD((c.relaxed_timers = true, c.tx_interval_ms = 0));
    BAD((c.relaxed_timers = true, c.tx_burst_us = 0));
    BAD(c.fop_timeout_ms = 0);
    BAD(c.history_depth = ERPS_HISTORY_MAX + 1);
    BAD(c.log_level = (erps_log_level_t)9);
#undef BAD
    erps_config_init(NULL); /* no crash */
    mock_cfg(&c, ERPS_ROLE_NONE, ERPS_PORT0);
    c.tx_version = 0;
    CHECK_EQ(erps_config_validate(&c, NULL, 0), ERPS_OK);
}

TEST(runtime_setters)
{
    struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
    CHECK_EQ(erps_set_timers(m->e, 120000, 300, 200), ERPS_OK);
    CHECK_EQ(erps_get_config(m->e)->wtr_ms, 120000);
    CHECK_EQ(erps_set_timers(m->e, 1, 300, 200), ERPS_E_CONFIG);
    CHECK_EQ(erps_set_timers(NULL, 1, 1, 1), ERPS_E_INVAL);
    CHECK_EQ(erps_set_revertive(NULL, true), ERPS_E_INVAL);
    erps_set_log_level(m->e, ERPS_LOG_ERR);
    CHECK_EQ(erps_get_config(m->e)->log_level, ERPS_LOG_ERR);
    CHECK(erps_get_user(m->e) == m);
    CHECK(erps_get_user(NULL) == NULL);
    CHECK(erps_get_config(NULL) == NULL);
    CHECK_EQ(erps_send_flush_event(NULL, 0), ERPS_E_INVAL);
    erps_stop(m->e, m->now, false);
    CHECK_EQ(erps_send_flush_event(m->e, m->now), ERPS_E_STATE);
    erps_reject_t why;
    CHECK_EQ(erps_command(m->e, ERPS_CMD_CLEAR, ERPS_PORT0, m->now, &why), ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_NOT_RUNNING);
    mock_free(m);
}
