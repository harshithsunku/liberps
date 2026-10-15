/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Behaviour pinned down after the second independent review:
 *  - Table 10-2 row 71 applies to every node in Pending, including the RPL owner and neighbour
 *    (unlike row 15). That keeps exactly one block in the ring: the node with the highest node ID.
 *  - Ring cold start: row 1 starts WTR at the owner, so a ring whose highest node ID is not the
 *    owner stays split in two segments for the whole WTR; init_wtb (App. VIII Table VIII.1 row 1)
 *    shortens that to WTB.
 */
#include "simutil.h"

static unsigned blocked_ports(erps_sim_t *s)
{
    unsigned n = 0;
    for (unsigned i = 0; i < erps_sim_nodes(s); i++)
        for (int p = 0; p < 2; p++)
            n += erps_sim_port_blocked(s, i, (erps_port_t)p);
    return n;
}

/* Non-revertive cold start: owner (G, 75) and neighbour (A, 81) open the RPL on the R-APS(NR) of
 * C (89, highest), which keeps the single block; Clear at the owner moves it to the RPL. */
TEST(review_nonrevertive_cold_start_single_block_then_clear)
{
    erps_sim_t *s = app3_new(false);
    erps_sim_run_for(s, ERPS_SEC(30));
    sim_show(s);
    CHECK_EQ(blocked_ports(s), 1);
    CHECK(erps_sim_port_blocked(s, NC, ERPS_PORT0));
    for (unsigned i = 0; i < 7; i++)
        CHECK_EQ(erps_sim_node_state(s, i), ERPS_STATE_PENDING);
    erps_sim_run_for(s, ERPS_SEC(600));
    CHECK_EQ(blocked_ports(s), 1); /* non-revertive: no automatic change */
    CHECK_EQ(erps_sim_command(s, NG, ERPS_CMD_CLEAR, ERPS_PORT_NONE, NULL), ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);
    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* An RPL neighbour in Pending executes row 71 like any node: on R-APS(NR) from a higher node ID it
 * unblocks; the higher node keeps the block. */
TEST(review_neighbour_row71_in_pending)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(2));
    erps_status_t st;
    erps_get_status(erps_sim_node(s, NA), &st);
    CHECK_EQ(st.state, ERPS_STATE_PENDING);
    CHECK(!st.port[0].blocked); /* A (81) heard C (89) */
    CHECK(erps_sim_port_blocked(s, NC, ERPS_PORT0));
    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* Revertive cold start, literal row 1: two blocks (C's init block and the owner's RPL block) for
 * the WTR period; with init_wtb the ring is Idle within guard + 5 s. */
TEST(review_cold_start_wtr_vs_init_wtb)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(10));
    CHECK_EQ(blocked_ports(s), 2); /* ring segmented until WTR expiry (documented) */
    erps_sim_run_for(s, ERPS_SEC(300));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);
    erps_sim_destroy(s);

    erps_sim_cfg_t c;
    erps_sim_cfg_init(&c);
    c.nodes = 7;
    c.owner = NG;
    c.node_template.init_wtb = true;
    s = erps_sim_create(&c);
    REQUIRE(s);
    for (unsigned i = 0; i < 7; i++)
        erps_sim_node_config(s, i)->node_id[5] = app3_ids[i];
    REQUIRE(erps_sim_start(s) == ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(7));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);
    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}
