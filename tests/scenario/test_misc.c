/* SPDX-License-Identifier: Apache-2.0 */
#include "simutil.h"

/*
 * Miscellaneous scenario tests:
 * 14. Two-node ring
 * 15. Node failure of a middle node
 * 16. Failure of the RPL owner node
 * 17. FOP-PM with two RPL owners
 * 18. Guard timer race (App. IV.2)
 * 19. Long-running stability (1 hour virtual time)
 * 20. Hold-off timer filtering
 */

/* 14. Two-node ring: init, link failure on each link, recovery */
TEST(misc_two_node_ring)
{
    erps_sim_cfg_t c;
    erps_sim_cfg_init(&c);
    c.nodes = 2;
    c.owner = 1;
    c.neighbour = true;

    erps_sim_t *s = erps_sim_create(&c);
    REQUIRE(s != NULL);
    REQUIRE(erps_sim_start(s) == ERPS_OK);

    /* Run 301s to reach Idle: node 1 (owner) port 1 blocked, node 0 (neighbour) port 0 blocked */
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, 1, 1, 0, 0);

    /* Fail non-RPL link (link 0 between node 0 port 1 and node 1 port 0) */
    erps_sim_link_set(s, 0, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    EXPECT_RING(s, ERPS_STATE_PROTECTION, 0, 1, 1, 0);

    /* Recover link 0 */
    erps_sim_link_set(s, 0, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(307));
    EXPECT_RING(s, ERPS_STATE_IDLE, 1, 1, 0, 0);

    /* Fail RPL link (link 1 between node 1 port 1 and node 0 port 0) */
    erps_sim_link_set(s, 1, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    EXPECT_RING(s, ERPS_STATE_PROTECTION, 1, 1, 0, 0);

    /* Recover link 1 */
    erps_sim_link_set(s, 1, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(307));
    EXPECT_RING(s, ERPS_STATE_IDLE, 1, 1, 0, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 15. Node failure of a middle node: neighbours protect; node recovers -> Idle */
TEST(misc_middle_node_failure_and_recovery)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Power off middle node NC (node 2) */
    erps_sim_node_set_up(s, NC, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    CHECK_EQ(erps_sim_node_state(s, NC), ERPS_STATE_INIT);

    /* Neighbours NB (node 1, port 1 facing NC) and ND (node 3, port 0 facing NC)
     * detect SSF, declare SF, and block their ports. RPL unblocks at NG and NA. */
    CHECK(erps_sim_port_blocked(s, NB, ERPS_PORT1));
    CHECK(erps_sim_port_blocked(s, ND, ERPS_PORT0));
    CHECK(!erps_sim_port_blocked(s, NG, ERPS_PORT1));
    CHECK(!erps_sim_port_blocked(s, NA, ERPS_PORT0));

    /* All live nodes are in Protection */
    for (unsigned i = 0; i < 7; i++) {
        if (i != NC)
            CHECK_EQ(erps_sim_node_state(s, i), ERPS_STATE_PROTECTION);
    }

    /* Node NC recovers */
    erps_sim_node_set_up(s, NC, true);
    erps_sim_run_for(s, ERPS_SEC(6));

    /* Reversion after WTR */
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 16. Failure of the RPL owner node itself, and recovery */
TEST(misc_owner_node_failure_and_recovery)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Power off RPL owner node NG (node 6) */
    erps_sim_node_set_up(s, NG, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    CHECK_EQ(erps_sim_node_state(s, NG), ERPS_STATE_INIT);

    /* Neighbours NF (port 1) and NA (port 0, neighbour end of RPL) detect SSF */
    CHECK(erps_sim_port_blocked(s, NF, ERPS_PORT1));
    CHECK(erps_sim_port_blocked(s, NA, ERPS_PORT0));

    for (unsigned i = 0; i < 6; i++)
        CHECK_EQ(erps_sim_node_state(s, i), ERPS_STATE_PROTECTION);

    /* RPL owner recovers */
    erps_sim_node_set_up(s, NG, true);
    erps_sim_run_for(s, ERPS_SEC(6));

    /* WTR runs on recovered owner, then ring reverts to Idle */
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 17. FOP-PM: configure two RPL owners on the ring -> owners raise ERPS_EV_FOP_PM */
struct fop_hook_ctx {
    unsigned raised[7];
};

static void fop_event_hook(void *ctx, unsigned node, const erps_event_t *ev)
{
    struct fop_hook_ctx *h = ctx;
    if (ev->type == ERPS_EV_FOP_PM && ev->u.fop.raised && node < 7)
        h->raised[node]++;
}

TEST(misc_fop_pm_dual_owner)
{
    erps_sim_cfg_t c;
    erps_sim_cfg_init(&c);
    c.nodes = 7;
    c.owner = NG;
    c.neighbour = true;

    erps_sim_t *s = erps_sim_create(&c);
    REQUIRE(s != NULL);

    for (unsigned i = 0; i < 7; i++)
        erps_sim_node_config(s, i)->node_id[5] = app3_ids[i];

    /* Configure a second node as RPL owner: NC (node 2) */
    erps_config_t *cfg_c = erps_sim_node_config(s, NC);
    cfg_c->role = ERPS_ROLE_RPL_OWNER;
    cfg_c->rpl_port = ERPS_PORT1;

    struct fop_hook_ctx hook_data = { { 0 } };
    erps_sim_set_event_hook(s, fop_event_hook, &hook_data);

    REQUIRE(erps_sim_start(s) == ERPS_OK);

    /* Run past WTR (300 s): both owners revert, block their RPL, and transmit (NR,RB).
     * Upon receiving the other owner's (NR,RB), both owners detect FOP-PM (§10.4). */
    erps_sim_run_for(s, ERPS_SEC(301));

    CHECK(hook_data.raised[NG] > 0);
    CHECK(hook_data.raised[NC] > 0);

    erps_status_t st_g, st_c, st_a;
    erps_get_status(erps_sim_node(s, NG), &st_g);
    erps_get_status(erps_sim_node(s, NC), &st_c);
    erps_get_status(erps_sim_node(s, NA), &st_a);

    CHECK(st_g.fop_pm);
    CHECK(st_c.fop_pm);
    CHECK(!st_a.fop_pm);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 18. Guard timer race (Appendix IV.2 / Fig IV.1): adjacent links fail and recover simultaneously */
TEST(misc_guard_timer_race)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Two adjacent links fail: link 1 (B-C) and link 2 (C-D) */
    erps_sim_link_set(s, 1, ERPS_SIM_BOTH, false);
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    EXPECT_RING(s, ERPS_STATE_PROTECTION, NB, 1, NC, 0, NC, 1, ND, 0);

    /* Both links recover at the exact same instant */
    erps_sim_link_set(s, 1, ERPS_SIM_BOTH, true);
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, true);

    /* Guard timer discards stale R-APS messages: no loop must form at any time */
    erps_sim_run_for(s, ERPS_SEC(6));
    erps_sim_run_for(s, ERPS_SEC(301));

    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);
    CHECK_NO_LOOPS(s);
    CHECK_EQ(erps_sim_loop_violations(s), 0);

    erps_sim_destroy(s);
}

/* 19. Long-running stability: Idle ring for 1 hour of virtual time */
TEST(misc_long_running_stability)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    erps_counters_t c_before[7];
    uint64_t flushes_before[7];
    for (unsigned i = 0; i < 7; i++) {
        erps_get_counters(erps_sim_node(s, i), &c_before[i]);
        flushes_before[i] = erps_sim_node_flushes(s, i);
    }

    /* Advance virtual clock by 1 hour (3600 s) */
    erps_sim_run_for(s, ERPS_SEC(3600));

    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* No state changes on any node during the hour */
    for (unsigned i = 0; i < 7; i++) {
        erps_counters_t c_now;
        erps_get_counters(erps_sim_node(s, i), &c_now);
        CHECK_EQ(c_now.state_changes, c_before[i].state_changes);
    }

    /* No flushes on any node during the hour */
    for (unsigned i = 0; i < 7; i++)
        CHECK_EQ(erps_sim_node_flushes(s, i), flushes_before[i]);

    /* R-APS(NR,RB) kept flowing from the owner (~ every 5 s, so ~720 frames per port) */
    erps_counters_t c_g;
    erps_get_counters(erps_sim_node(s, NG), &c_g);
    uint64_t diff_p0 = c_g.port[0].tx_raps[ERPS_MSG_NR_RB] - c_before[NG].port[0].tx_raps[ERPS_MSG_NR_RB];
    uint64_t diff_p1 = c_g.port[1].tx_raps[ERPS_MSG_NR_RB] - c_before[NG].port[1].tx_raps[ERPS_MSG_NR_RB];
    CHECK(diff_p0 >= 710 && diff_p0 <= 730);
    CHECK(diff_p1 >= 710 && diff_p1 <= 730);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 20. Hold-off: failure shorter than 500 ms causes no switch; longer than 500 ms does */
TEST(misc_holdoff_filtering)
{
    erps_sim_cfg_t c;
    erps_sim_cfg_init(&c);
    c.nodes = 7;
    c.owner = NG;
    c.neighbour = true;
    c.node_template.holdoff_ms = 500;

    erps_sim_t *s = erps_sim_create(&c);
    REQUIRE(s != NULL);

    for (unsigned i = 0; i < 7; i++) {
        erps_sim_node_config(s, i)->node_id[5] = app3_ids[i];
        erps_sim_node_config(s, i)->holdoff_ms = 500;
    }
    REQUIRE(erps_sim_start(s) == ERPS_OK);

    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Failure shorter than 500 ms: link 2 down for 300 ms */
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_MSEC(300));
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(1));

    /* No protection switch: ring remains in Idle */
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);
    CHECK_EQ(erps_sim_node_state(s, NC), ERPS_STATE_IDLE);
    CHECK_EQ(erps_sim_node_state(s, ND), ERPS_STATE_IDLE);

    /* Failure longer than 500 ms: link 2 down for 700 ms */
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_MSEC(700));

    /* Hold-off expired and declared SF: protection switch occurred */
    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1, ND, 0);

    /* Recover link 2 and return to Idle */
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(307));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}
