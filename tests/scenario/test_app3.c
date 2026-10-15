/* SPDX-License-Identifier: Apache-2.0 */
#include "simutil.h"

/*
 * Appendix III Scenarios (7-node ring A..G, G owner, A neighbour).
 * Node IDs: NA=81, NB=26, NC=89, ND=62, NE=71, NF=31, NG=75.
 * Links: 0=A-B, 1=B-C, 2=C-D, 3=D-E, 4=E-F, 5=F-G, 6=G-A(RPL).
 */

/* 1. Scenario A, single link failure C-D (link 2 both directions) */
TEST(app3_scenario_a_single_link_failure)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    uint64_t flushes_before[7];
    for (unsigned i = 0; i < 7; i++)
        flushes_before[i] = erps_sim_node_flushes(s, i);

    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1, ND, 0);

    for (unsigned i = 0; i < 7; i++)
        CHECK(erps_sim_node_flushes(s, i) > flushes_before[i]);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 2. Scenario A recovery, revertive (Fig III.2) */
TEST(app3_scenario_a_recovery_revertive)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1, ND, 0);

    /* Link recovers */
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, true);

    /* After 6s, guard timer (500 ms) has expired and the next 5s periodic R-APS(NR)
     * has arrived: C(89) > D(62), so D unblocks port 0 on receiving NR from C; C stays blocked.
     * Nodes are in Pending state, and owner started WTR (300 s). */
    erps_sim_run_for(s, ERPS_SEC(6));
    EXPECT_RING(s, ERPS_STATE_PENDING, NC, 1);

    /* At ~WTR - 1s (299 s total from recovery, so advance 293 s more),
     * owner is still Pending and RPL is still unblocked. */
    erps_sim_run_for(s, ERPS_SEC(293));
    CHECK_EQ(erps_sim_node_state(s, NG), ERPS_STATE_PENDING);
    EXPECT_RING(s, ERPS_STATE_PENDING, NC, 1);

    /* Advance past WTR: owner reverts, blocks RPL, ring returns to Idle */
    erps_sim_run_for(s, ERPS_SEC(5));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 3. Scenario A recovery, non-revertive (Fig III.3) */
TEST(app3_scenario_a_recovery_non_revertive)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    for (unsigned i = 0; i < 7; i++)
        erps_set_revertive(erps_sim_node(s, i), false);

    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1, ND, 0);

    /* Link recovers */
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, true);

    /* Guard timer expires (500 ms) and next periodic frame arrives at 5s.
     * C has logical ID 89, D has logical ID 62.
     * C > D, so D receives NR from higher node ID C and unblocks port 0.
     * C keeps its port 1 blocked.
     * Owner does not start WTR in non-revertive mode.
     * Ring stays Pending with NC port 1 blocked, RPL unblocked. */
    erps_sim_run_for(s, ERPS_SEC(6));
    EXPECT_RING(s, ERPS_STATE_PENDING, NC, 1);

    /* Even after a long time (e.g. 400 s), ring stays Pending */
    erps_sim_run_for(s, ERPS_SEC(400));
    EXPECT_RING(s, ERPS_STATE_PENDING, NC, 1);

    /* Clear command on owner triggers reversion */
    erps_reject_t why = ERPS_REJECT_NONE;
    erps_err_t rc = erps_sim_command(s, NG, ERPS_CMD_CLEAR, ERPS_PORT_NONE, &why);
    CHECK_EQ(rc, ERPS_OK);

    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 4. Scenario B, unidirectional failure (only direction D->C failed, link 2 CCW) */
TEST(app3_scenario_b_unidirectional_failure)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Fail direction D -> C (CCW on link 2) */
    erps_sim_link_set(s, 2, ERPS_SIM_CCW, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    /* Only C detects SF on port 1; D detects nothing.
     * All nodes in Protection. Only NC port 1 is blocked. */
    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1);
    CHECK(!erps_sim_port_blocked(s, ND, ERPS_PORT0));

    erps_status_t st_d;
    erps_get_status(erps_sim_node(s, ND), &st_d);
    CHECK(!st_d.port[0].sf);

    /* Recovery of the unidirectional link */
    erps_sim_link_set(s, 2, ERPS_SIM_CCW, true);
    erps_sim_run_for(s, ERPS_SEC(1));

    /* C detects clearing of SF, sends NR, owner starts WTR */
    EXPECT_RING(s, ERPS_STATE_PENDING, NC, 1);

    /* Wait for WTR expiry */
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 5. Scenario C, RPL failure (link 6): R-APS(SF,DNF) and no node flushes */
TEST(app3_scenario_c_rpl_failure)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    uint64_t flushes_before[7];
    for (unsigned i = 0; i < 7; i++)
        flushes_before[i] = erps_sim_node_flushes(s, i);

    /* Fail link 6 (RPL link between NG port 1 and NA port 0) */
    erps_sim_link_set(s, 6, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    EXPECT_RING(s, ERPS_STATE_PROTECTION, NG, 1, NA, 0);

    /* Both NG and NA had RPL already blocked, so they send SF with DNF */
    erps_status_t st_g, st_a;
    erps_get_status(erps_sim_node(s, NG), &st_g);
    erps_get_status(erps_sim_node(s, NA), &st_a);
    CHECK(st_g.tx_msg.dnf);
    CHECK(st_a.tx_msg.dnf);

    /* No node flushes on RPL failure */
    for (unsigned i = 0; i < 7; i++)
        CHECK_EQ(erps_sim_node_flushes(s, i), flushes_before[i]);

    /* Recovery */
    erps_sim_link_set(s, 6, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(1));

    /* Owner reverts after WTR with (NR,RB,DNF) */
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    erps_get_status(erps_sim_node(s, NG), &st_g);
    CHECK(st_g.tx_msg.dnf);

    /* Still no node flushed through the entire recovery */
    for (unsigned i = 0; i < 7; i++)
        CHECK_EQ(erps_sim_node_flushes(s, i), flushes_before[i]);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 6. Scenario D, multiple failures A-B, E-F, C-D; then A-B and E-F recover */
TEST(app3_scenario_d_multiple_failures)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Fail link 0 (A-B), link 2 (C-D), link 4 (E-F) */
    erps_sim_link_set(s, 0, ERPS_SIM_BOTH, false);
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_link_set(s, 4, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    EXPECT_RING(s, ERPS_STATE_PROTECTION, NA, 1, NB, 0, NC, 1, ND, 0, NE, 1, NF, 0);

    /* A-B and E-F recover nearly simultaneously; C-D stays failed */
    erps_sim_link_set(s, 0, ERPS_SIM_BOTH, true);
    erps_sim_link_set(s, 4, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(20));

    /* End state: all nodes in Protection, only C-D blocked */
    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1, ND, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}
