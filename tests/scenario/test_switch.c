/* SPDX-License-Identifier: Apache-2.0 */
#include "simutil.h"

/*
 * Switch command scenarios (clauses 10.2.3, 10.2.4, 10.1.9, 10.1.13).
 * MS, FS, Clear, and backward compatibility.
 */

/* 7. MS from Idle, then Clear -> Pending -> owner reverts after WTB, not WTR */
TEST(switch_ms_from_idle_and_clear_wtb)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Issue MS on a non-RPL node port (NC port 1) */
    erps_reject_t why = ERPS_REJECT_NONE;
    erps_err_t rc = erps_sim_command(s, NC, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT1, &why);
    CHECK_EQ(rc, ERPS_OK);

    erps_sim_run_for(s, ERPS_SEC(1));
    /* Port is blocked, RPL unblocked, all nodes in Manual Switch */
    EXPECT_RING(s, ERPS_STATE_MANUAL_SWITCH, NC, 1);

    /* Clear the MS on NC */
    rc = erps_sim_command(s, NC, ERPS_CMD_CLEAR, ERPS_PORT_NONE, &why);
    CHECK_EQ(rc, ERPS_OK);

    /* NC starts guard timer (500 ms) and sends NR.
     * Owner receives NR, enters Pending and starts WTB (guard + 5s = 5500 ms). */
    erps_sim_run_for(s, ERPS_SEC(1));
    CHECK_EQ(erps_sim_node_state(s, NG), ERPS_STATE_PENDING);

    erps_status_t st_g;
    erps_get_status(erps_sim_node(s, NG), &st_g);
    CHECK(st_g.wtb_running);
    CHECK(!st_g.wtr_running);

    /* At t = 4s after Clear, WTB is still running (~1.5s left) and owner is still Pending */
    erps_sim_run_for(s, ERPS_SEC(3));
    CHECK_EQ(erps_sim_node_state(s, NG), ERPS_STATE_PENDING);

    /* Advance past WTB expiry (at 5.5s after Clear): owner reverts to Idle after WTB, not WTR */
    erps_sim_run_for(s, ERPS_SEC(3));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 8. Second MS elsewhere while MS exists is rejected */
TEST(switch_second_ms_rejected)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* First MS on NC port 1 */
    CHECK_EQ(erps_sim_command(s, NC, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT1, NULL), ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_MANUAL_SWITCH, NC, 1);

    /* Second MS on ND port 1 is rejected (§10.2.3 rule 1) */
    erps_reject_t why = ERPS_REJECT_NONE;
    erps_err_t rc = erps_sim_command(s, ND, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT1, &why);
    CHECK_EQ(rc, ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_MS_IN_RING);

    /* First MS is still in effect */
    EXPECT_RING(s, ERPS_STATE_MANUAL_SWITCH, NC, 1);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 9. Multiple FS segment the ring; clear one by one, revert after WTB */
TEST(switch_fs_multiple_and_clear_wtb)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* First FS on NC port 1 */
    CHECK_EQ(erps_sim_command(s, NC, ERPS_CMD_FORCED_SWITCH, ERPS_PORT1, NULL), ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_FORCED_SWITCH, NC, 1);

    /* Second FS on NE port 1 is accepted -> ring segmented */
    CHECK_EQ(erps_sim_command(s, NE, ERPS_CMD_FORCED_SWITCH, ERPS_PORT1, NULL), ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_FORCED_SWITCH, NC, 1, NE, 1);

    /* Clear first FS on NC: ring stays FS-blocked by NE's FS */
    CHECK_EQ(erps_sim_command(s, NC, ERPS_CMD_CLEAR, ERPS_PORT_NONE, NULL), ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(6));
    EXPECT_RING(s, ERPS_STATE_FORCED_SWITCH, NE, 1);

    /* Clear second FS on NE: reversion begins */
    CHECK_EQ(erps_sim_command(s, NE, ERPS_CMD_CLEAR, ERPS_PORT_NONE, NULL), ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(1));

    erps_status_t st_g;
    erps_get_status(erps_sim_node(s, NG), &st_g);
    CHECK(st_g.wtb_running);
    CHECK(!st_g.wtr_running);

    /* Revert after WTB */
    erps_sim_run_for(s, ERPS_SEC(6));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 10. FS overrides existing SF (rows 17/18), SF ignored in FS (row 47), restored after Clear */
TEST(switch_fs_overrides_sf_and_ignored_sf_restored)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Link 2 (C-D) fails -> Protection */
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1, ND, 0);

    /* FS on NC port 1: NC is in Protection with local SF.
     * Table 10-2 row 17: requested port 1 already blocked -> Tx(FS, DNF), next D.
     * Other nodes in Protection (e.g. ND) receive R-APS(FS) and execute row 18:
     * unblock ring ports, stop Tx, next D. */
    CHECK_EQ(erps_sim_command(s, NC, ERPS_CMD_FORCED_SWITCH, ERPS_PORT1, NULL), ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(1));

    EXPECT_RING(s, ERPS_STATE_FORCED_SWITCH, NC, 1);

    /* Check row execution: NC executed row 17, ND executed row 18 */
    uint8_t rows_c[9], rows_d[9];
    erps_get_row_coverage(erps_sim_node(s, NC), rows_c);
    erps_get_row_coverage(erps_sim_node(s, ND), rows_d);
    CHECK((rows_c[17 / 8] & (1u << (17 % 8))) != 0);
    CHECK((rows_d[18 / 8] & (1u << (18 % 8))) != 0);

    /* SF arriving during FS is ignored (row 47): fail link 1 (B-C) while FS is active.
     * Node NC detects local SF on port 0, but remains in Forced Switch; only NC port 1 stays blocked. */
    erps_sim_link_set(s, 1, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_FORCED_SWITCH, NC, 1);

    /* Recover link 1 */
    erps_sim_link_set(s, 1, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_FORCED_SWITCH, NC, 1);

    /* Clear the FS on NC: NC leaves FS (row 44).
     * The persistent local SF on link 2 is acted upon (row 61 / 63),
     * and the ring returns to Protection. */
    CHECK_EQ(erps_sim_command(s, NC, ERPS_CMD_CLEAR, ERPS_PORT_NONE, NULL), ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(6));

    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1, ND, 0);

    /* Restore link 2 and let ring revert to Idle */
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(307));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 11. MS is pre-empted by SF elsewhere; after SF recovery, MS is not re-applied */
TEST(switch_ms_preempted_by_sf)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Local MS on NC port 1 */
    CHECK_EQ(erps_sim_command(s, NC, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT1, NULL), ERPS_OK);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_MANUAL_SWITCH, NC, 1);

    erps_status_t st_c;
    erps_get_status(erps_sim_node(s, NC), &st_c);
    CHECK(st_c.ms_active);

    /* SF occurs elsewhere: link 4 (E-F) fails.
     * R-APS(SF) > MS: NC leaves MS, clears local MS request (§10.2.3 rule 3, row 35). */
    erps_sim_link_set(s, 4, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));

    EXPECT_RING(s, ERPS_STATE_PROTECTION, NE, 1, NF, 0);

    erps_get_status(erps_sim_node(s, NC), &st_c);
    CHECK(!st_c.ms_active);

    /* Recover link 4: ring reverts and MS is NOT re-applied */
    erps_sim_link_set(s, 4, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(307));

    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 12. Clear on a node with no local command and not the owner is rejected */
TEST(switch_clear_rejected_when_not_applicable)
{
    erps_sim_t *s = app3_new(true);
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Clear on NC (non-owner, no local FS/MS active) is rejected (§10.1.9) */
    erps_reject_t why = ERPS_REJECT_NONE;
    erps_err_t rc = erps_sim_command(s, NC, ERPS_CMD_CLEAR, ERPS_PORT_NONE, &why);
    CHECK_EQ(rc, ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_CLEAR_NOT_APPLICABLE);

    /* Even when in Protection, Clear on a node without local command is rejected */
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1, ND, 0);

    why = ERPS_REJECT_NONE;
    rc = erps_sim_command(s, NC, ERPS_CMD_CLEAR, ERPS_PORT_NONE, &why);
    CHECK_EQ(rc, ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_CLEAR_NOT_APPLICABLE);

    why = ERPS_REJECT_NONE;
    rc = erps_sim_command(s, NB, ERPS_CMD_CLEAR, ERPS_PORT_NONE, &why);
    CHECK_EQ(rc, ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_CLEAR_NOT_APPLICABLE);

    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(307));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}

/* 13. compat_version = 1: FS and MS rejected; non-revertive ignored */
TEST(switch_compat_v1_rejects_commands_forces_revertive)
{
    erps_sim_cfg_t c;
    erps_sim_cfg_init(&c);
    c.nodes = 7;
    c.owner = NG;
    c.neighbour = true;
    c.node_template.revertive = false; /* requested non-revertive */

    erps_sim_t *s = erps_sim_create(&c);
    REQUIRE(s != NULL);

    for (unsigned i = 0; i < 7; i++) {
        erps_config_t *cfg = erps_sim_node_config(s, i);
        cfg->node_id[5] = app3_ids[i];
        cfg->compat_version = 1;
        cfg->revertive = false;
    }
    REQUIRE(erps_sim_start(s) == ERPS_OK);

    /* Revertive is forced true on all nodes (§10.1.13 a2) */
    for (unsigned i = 0; i < 7; i++) {
        erps_status_t st;
        erps_get_status(erps_sim_node(s, i), &st);
        CHECK(st.revertive);
    }

    /* FS and MS are rejected on compat_version = 1 (§10.1.13 a1) */
    erps_reject_t why = ERPS_REJECT_NONE;
    erps_err_t rc = erps_sim_command(s, NC, ERPS_CMD_MANUAL_SWITCH, ERPS_PORT1, &why);
    CHECK_EQ(rc, ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_COMPAT_V1);

    why = ERPS_REJECT_NONE;
    rc = erps_sim_command(s, NC, ERPS_CMD_FORCED_SWITCH, ERPS_PORT1, &why);
    CHECK_EQ(rc, ERPS_E_REJECTED);
    CHECK_EQ(why, ERPS_REJECT_COMPAT_V1);

    /* Run to Idle (row 1 started WTR because revertive was forced on) */
    erps_sim_run_for(s, ERPS_SEC(301));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    /* Link failure and recovery: ring reverts because revertive is forced */
    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, false);
    erps_sim_run_for(s, ERPS_SEC(1));
    EXPECT_RING(s, ERPS_STATE_PROTECTION, NC, 1, ND, 0);

    erps_sim_link_set(s, 2, ERPS_SIM_BOTH, true);
    erps_sim_run_for(s, ERPS_SEC(6));
    EXPECT_RING(s, ERPS_STATE_PENDING, NC, 1);

    /* Wait for WTR expiry -> reverts to Idle without any operator Clear */
    erps_sim_run_for(s, ERPS_SEC(300));
    EXPECT_RING(s, ERPS_STATE_IDLE, NG, 1, NA, 0);

    CHECK_NO_LOOPS(s);
    erps_sim_destroy(s);
}
