/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Table 10-2, row by row. Each test forces the node state and port state, runs one row through
 * the state machine (bypassing the priority logic) and checks actions and next state against
 * G.8032 (03/2020) Table 10-2.
 */
#include "mock.h"

#define A ERPS_STATE_IDLE
#define B ERPS_STATE_PROTECTION
#define C ERPS_STATE_MANUAL_SWITCH
#define D ERPS_STATE_FORCED_SWITCH
#define E ERPS_STATE_PENDING

/* Independent transcription of the Table 10-2 (state, request) -> row mapping. */
TEST(fsm_row_numbers_match_table_10_2)
{
    static const erps_req_type_t order[14] = {
        ERPS_REQ_CLEAR,       ERPS_REQ_FS,          ERPS_REQ_RAPS_FS,     ERPS_REQ_LOCAL_SF,
        ERPS_REQ_LOCAL_CLEAR_SF, ERPS_REQ_RAPS_SF,  ERPS_REQ_RAPS_MS,     ERPS_REQ_MS,
        ERPS_REQ_WTR_EXPIRES, ERPS_REQ_WTR_RUNNING, ERPS_REQ_WTB_EXPIRES, ERPS_REQ_WTB_RUNNING,
        ERPS_REQ_RAPS_NR_RB,  ERPS_REQ_RAPS_NR,
    };
    static const erps_state_t states[5] = { A, B, C, D, E };
    /* Rows 2..71: 14 requests per state, in Table 10-1 order. */
    uint8_t row = 2;
    for (int s = 0; s < 5; s++)
        for (int r = 0; r < 14; r++)
            CHECK_EQ(erps__fsm_row(states[s], order[r]), row++);
    CHECK_EQ(row, 72);
    CHECK_EQ(erps__fsm_row(ERPS_STATE_INIT, ERPS_REQ_CLEAR), 0);
    CHECK_EQ(erps__fsm_row(A, ERPS_REQ_NONE), 0);
}

/* ---- Row 1 --------------------------------------------------------------------------- */

TEST(row01_init_owner_revertive)
{
    struct mock *m = mock_new(ERPS_ROLE_RPL_OWNER, ERPS_PORT1);
    REQUIRE(erps_start(m->e, m->now) == ERPS_OK);
    CHECK_EQ(m->e->state, E);
    CHECK_PORTS(m, false, true);
    CHECK_TX(m, ERPS_RAPS_NR, false, false, 1);
    CHECK(m->e->wtr_deadline != ERPS_TIME_NEVER);
    CHECK_EQ(m->e->last_row == 1 || m->e->last_row == 67, 1); /* settle may signal WTR running */
    CHECK_EQ(m->port_calls[0], 1);
    CHECK_EQ(m->port_calls[1], 1);
    mock_free(m);
}

TEST(row01_init_owner_nonrevertive)
{
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
    c.revertive = false;
    struct mock *m = mock_new_cfg(&c);
    REQUIRE(erps_start(m->e, m->now) == ERPS_OK);
    CHECK_EQ(m->e->state, E);
    CHECK_PORTS(m, true, false);
    CHECK_TX(m, ERPS_RAPS_NR, false, false, 0);
    CHECK(m->e->wtr_deadline == ERPS_TIME_NEVER);
    mock_free(m);
}

TEST(row01_init_neighbour_and_normal)
{
    struct mock *m = mock_started(ERPS_ROLE_RPL_NEIGHBOUR, ERPS_PORT1);
    CHECK_EQ(m->e->state, E);
    CHECK_PORTS(m, false, true);
    CHECK_TX(m, ERPS_RAPS_NR, false, false, 1);
    CHECK(m->e->wtr_deadline == ERPS_TIME_NEVER);
    mock_free(m);

    m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    CHECK_EQ(m->e->state, E);
    CHECK_PORTS(m, true, false); /* block one ring port (port 0), unblock the other */
    CHECK_TX(m, ERPS_RAPS_NR, false, false, 0);
    CHECK(m->e->guard_deadline == ERPS_TIME_NEVER);
    mock_free(m);
}

/* ---- FS rows 3, 17, 31, 59 and 45 ----------------------------------------------------------- */

static void fs_row(erps_state_t st, uint8_t row, erps_role_t role)
{
    /* requested port not yet blocked: block, Tx(FS), unblock other, flush */
    struct mock *m = mock_started(role, ERPS_PORT1);
    mock_setup(m, st, false, true);
    m->e->wtr_deadline = m->now + 1000;
    m->e->wtb_deadline = m->now + 1000;
    run_row(m, mk_req(ERPS_REQ_FS, ERPS_PORT0));
    CHECK_EQ(m->e->last_row, row);
    CHECK_EQ(m->e->state, D);
    CHECK_PORTS(m, true, false);
    CHECK_TX(m, ERPS_RAPS_FS, false, false, 0);
    CHECK_EQ(m->flushes, 1);
    bool stops = row == 59 && role == ERPS_ROLE_RPL_OWNER;
    CHECK_EQ(m->e->wtr_deadline == ERPS_TIME_NEVER, stops);
    CHECK_EQ(m->e->wtb_deadline == ERPS_TIME_NEVER, stops);

    /* requested port already blocked: Tx(FS,DNF), unblock other, no flush */
    mock_setup(m, st, false, true);
    run_row(m, mk_req(ERPS_REQ_FS, ERPS_PORT1));
    CHECK_EQ(m->e->last_row, row);
    CHECK_EQ(m->e->state, D);
    CHECK_PORTS(m, false, true);
    CHECK_TX(m, ERPS_RAPS_FS, false, true, 1);
    CHECK_EQ(m->flushes, 0);
    mock_free(m);
}

TEST(row03_17_31_59_forced_switch)
{
    fs_row(A, 3, ERPS_ROLE_NONE);
    fs_row(B, 17, ERPS_ROLE_NONE);
    fs_row(C, 31, ERPS_ROLE_NONE);
    fs_row(E, 59, ERPS_ROLE_NONE);
    fs_row(E, 59, ERPS_ROLE_RPL_OWNER);
}

TEST(row45_fs_in_fs)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    mock_setup(m, D, false, false);
    run_row(m, mk_req(ERPS_REQ_FS, ERPS_PORT1));
    CHECK_EQ(m->e->last_row, 45);
    CHECK_EQ(m->e->state, D);
    CHECK_PORTS(m, false, true); /* no "unblock non-requested" in row 45 */
    CHECK_TX(m, ERPS_RAPS_FS, false, false, 1);
    CHECK_EQ(m->flushes, 1);
    mock_free(m);
}

/* ---- R-APS(FS) rows 4, 18, 32, 60 ------------------------------------------------------------ */

TEST(row04_18_32_60_raps_fs)
{
    static const struct { erps_state_t st; uint8_t row; } t[] = { { A, 4 }, { B, 18 }, { C, 32 }, { E, 60 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
        mock_setup(m, t[i].st, true, true);
        m->e->wtr_deadline = m->now + 1000;
        run_row(m, mk_raps(ERPS_REQ_RAPS_FS, ERPS_PORT1, 0x10));
        CHECK_EQ(m->e->last_row, t[i].row);
        CHECK_EQ(m->e->state, D);
        CHECK_PORTS(m, false, false);
        CHECK(!m->e->tx.active);
        CHECK_EQ(m->e->wtr_deadline == ERPS_TIME_NEVER, t[i].row == 60);
        CHECK_EQ(m->flushes, 0);
        mock_free(m);
    }
}

/* ---- local SF rows 5, 19, 33, 61 ------------------------------------------------------------- */

static void sf_row(erps_state_t st, uint8_t row)
{
    struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
    /* failed port 1 not blocked: block it, Tx(SF), unblock non-failed (port 0 = RPL!), flush */
    mock_setup(m, st, true, false);
    m->e->port[1].sf = true;
    m->e->wtb_deadline = m->now + 5;
    erps_req_t r = mk_req(ERPS_REQ_LOCAL_SF, ERPS_PORT1);
    r.ports = 2;
    run_row(m, r);
    CHECK_EQ(m->e->last_row, row);
    CHECK_EQ(m->e->state, B);
    CHECK_PORTS(m, false, true);
    CHECK_TX(m, ERPS_RAPS_SF, false, false, 1);
    CHECK_EQ(m->flushes, 1);
    CHECK_EQ(m->e->wtb_deadline == ERPS_TIME_NEVER, row == 61);

    /* failed port already blocked (e.g. RPL failure at the owner): Tx(SF,DNF), no flush */
    mock_setup(m, st, false, true);
    run_row(m, r);
    CHECK_EQ(m->e->last_row, row);
    CHECK_PORTS(m, false, true);
    CHECK_TX(m, ERPS_RAPS_SF, false, true, 1);
    CHECK_EQ(m->flushes, 0);
    mock_free(m);
}

TEST(row05_19_33_61_local_sf)
{
    sf_row(A, 5);
    sf_row(B, 19);
    sf_row(C, 33);
    sf_row(E, 61);
}

TEST(row19_dual_sf_blocks_both_ports)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    mock_setup(m, B, true, false);
    m->e->port[0].sf = true;
    m->e->port[1].sf = true;
    erps_req_t r = mk_req(ERPS_REQ_LOCAL_SF, ERPS_PORT1);
    r.ports = 3;
    run_row(m, r);
    CHECK_EQ(m->e->last_row, 19);
    CHECK_PORTS(m, true, true);
    CHECK_TX(m, ERPS_RAPS_SF, false, false, 1);
    CHECK_EQ(m->flushes, 1);
    mock_free(m);
}

/* ---- rows 6, 34, 62, 48: local clear SF with no action -------------------------------------- */

TEST(row06_34_48_62_clear_sf_no_action)
{
    static const struct { erps_state_t st; uint8_t row; } t[] = { { A, 6 }, { C, 34 }, { D, 48 }, { E, 62 } };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
        mock_setup(m, t[i].st, true, false);
        run_row(m, mk_req(ERPS_REQ_LOCAL_CLEAR_SF, ERPS_PORT0));
        CHECK_EQ(m->e->last_row, t[i].row);
        CHECK_EQ(m->e->state, t[i].st);
        CHECK_PORTS(m, true, false);
        CHECK_EQ(m->ntx, 0);
        mock_free(m);
    }
}

/* ---- R-APS(SF) / R-APS(MS) rows 7, 8, 35, 63, 64 --------------------------------------------- */

TEST(row07_08_35_63_64_unblock_non_failed_stop_tx)
{
    static const struct { erps_state_t st; erps_req_type_t req; uint8_t row; erps_state_t next; } t[] = {
        { A, ERPS_REQ_RAPS_SF, 7, B },  { A, ERPS_REQ_RAPS_MS, 8, C }, { C, ERPS_REQ_RAPS_SF, 35, B },
        { E, ERPS_REQ_RAPS_SF, 63, B }, { E, ERPS_REQ_RAPS_MS, 64, C },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT1);
        mock_setup(m, t[i].st, true, true);
        m->e->port[0].sf = true; /* port 0 failed: stays blocked */
        m->e->wtr_deadline = m->now + 10;
        m->e->wtb_deadline = m->now + 10;
        run_row(m, mk_raps(t[i].req, ERPS_PORT1, 0x10));
        CHECK_EQ(m->e->last_row, t[i].row);
        CHECK_EQ(m->e->state, t[i].next);
        CHECK_PORTS(m, true, false);
        CHECK(!m->e->tx.active);
        bool stops = t[i].st == E;
        CHECK_EQ(m->e->wtr_deadline == ERPS_TIME_NEVER, stops);
        CHECK_EQ(m->e->wtb_deadline == ERPS_TIME_NEVER, stops);
        mock_free(m);
    }
}

/* ---- MS rows 9, 65 ---------------------------------------------------------------------------- */

TEST(row09_65_manual_switch)
{
    static const struct { erps_state_t st; uint8_t row; } t[] = { { A, 9 }, { E, 65 } };
    for (size_t i = 0; i < 2; i++) {
        struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
        mock_setup(m, t[i].st, true, false); /* RPL (port 0) blocked */
        m->e->wtr_deadline = m->now + 10;
        run_row(m, mk_req(ERPS_REQ_MS, ERPS_PORT1));
        CHECK_EQ(m->e->last_row, t[i].row);
        CHECK_EQ(m->e->state, C);
        CHECK_PORTS(m, false, true);
        CHECK_TX(m, ERPS_RAPS_MS, false, false, 1);
        CHECK_EQ(m->flushes, 1);
        CHECK_EQ(m->e->wtr_deadline == ERPS_TIME_NEVER, t[i].row == 65);

        /* MS on the already-blocked RPL port: (MS,DNF), no flush (App. VIII.2 d) */
        mock_setup(m, t[i].st, true, false);
        run_row(m, mk_req(ERPS_REQ_MS, ERPS_PORT0));
        CHECK_PORTS(m, true, false);
        CHECK_TX(m, ERPS_RAPS_MS, false, true, 0);
        CHECK_EQ(m->flushes, 0);
        mock_free(m);
    }
}

/* ---- Row 14: R-APS(NR,RB) in Idle --------------------------------------------------------- */

TEST(row14_raps_nr_rb_idle)
{
    /* normal node: unblock both, stop Tx */
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    mock_setup(m, A, true, false);
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR_RB, ERPS_PORT1, 0x10));
    CHECK_EQ(m->e->last_row, 14);
    CHECK_EQ(m->e->state, A);
    CHECK_PORTS(m, false, false);
    CHECK(!m->e->tx.active);
    mock_free(m);

    /* neighbour: RPL stays blocked */
    m = mock_started(ERPS_ROLE_RPL_NEIGHBOUR, ERPS_PORT0);
    mock_setup(m, A, true, true);
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR_RB, ERPS_PORT1, 0x10));
    CHECK_PORTS(m, true, false);
    CHECK(!m->e->tx.active);
    mock_free(m);

    /* owner: keeps transmitting */
    m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
    mock_setup(m, A, true, true);
    m->e->tx.active = true;
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR_RB, ERPS_PORT1, 0x10));
    CHECK_PORTS(m, true, false);
    CHECK(m->e->tx.active);
    mock_free(m);
}

/* ---- Row 15: R-APS(NR) in Idle ------------------------------------------------------------ */

TEST(row15_raps_nr_idle)
{
    /* normal node, higher remote ID: unblock non-failed, stop Tx */
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    mock_setup(m, A, true, false);
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR, ERPS_PORT1, 0x60));
    CHECK_EQ(m->e->last_row, 15);
    CHECK_PORTS(m, false, false);
    CHECK(!m->e->tx.active);
    /* lower remote ID: nothing */
    mock_setup(m, A, true, false);
    m->e->tx.active = true;
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR, ERPS_PORT1, 0x40));
    CHECK_PORTS(m, true, false);
    CHECK(m->e->tx.active);
    mock_free(m);
    /* owner / neighbour: nothing even for a higher ID */
    m = mock_started(ERPS_ROLE_RPL_NEIGHBOUR, ERPS_PORT0);
    mock_setup(m, A, true, false);
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR, ERPS_PORT1, 0x60));
    CHECK_PORTS(m, true, false);
    CHECK_EQ(m->e->state, A);
    mock_free(m);
}

/* ---- Row 20: local clear SF in Protection --------------------------------------------------- */

TEST(row20_clear_sf_in_protection)
{
    struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
    mock_setup(m, B, false, true);
    run_row(m, mk_req(ERPS_REQ_LOCAL_CLEAR_SF, ERPS_PORT1));
    CHECK_EQ(m->e->last_row, 20);
    CHECK_EQ(m->e->state, E);
    CHECK_PORTS(m, false, true); /* recovered port stays blocked */
    CHECK_TX(m, ERPS_RAPS_NR, false, false, 1);
    CHECK(m->e->guard_deadline == m->now + ERPS_MSEC(500));
    CHECK(m->e->wtr_deadline == m->now + ERPS_MSEC(300000));
    mock_free(m);

    /* non-owner: no WTR */
    m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    mock_setup(m, B, true, false);
    run_row(m, mk_req(ERPS_REQ_LOCAL_CLEAR_SF, ERPS_PORT0));
    CHECK(m->e->wtr_deadline == ERPS_TIME_NEVER);
    CHECK_TX(m, ERPS_RAPS_NR, false, false, 0);
    mock_free(m);

    /* owner, non-revertive: no WTR */
    erps_config_t c;
    mock_cfg(&c, ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
    c.revertive = false;
    m = mock_new_cfg(&c);
    erps_start(m->e, m->now);
    mock_setup(m, B, false, true);
    run_row(m, mk_req(ERPS_REQ_LOCAL_CLEAR_SF, ERPS_PORT1));
    CHECK(m->e->wtr_deadline == ERPS_TIME_NEVER);
    mock_free(m);
}

/* ---- Rows 28, 29, 42, 43, 56, 57: R-APS(NR[,RB]) leaving B/C/D ------------------------------ */

TEST(row28_29_42_43_56_57_nr_to_pending)
{
    static const struct { erps_state_t st; erps_req_type_t req; uint8_t row; int timer; } t[] = {
        { B, ERPS_REQ_RAPS_NR_RB, 28, 0 }, { B, ERPS_REQ_RAPS_NR, 29, 1 },
        { C, ERPS_REQ_RAPS_NR_RB, 42, 0 }, { C, ERPS_REQ_RAPS_NR, 43, 2 },
        { D, ERPS_REQ_RAPS_NR_RB, 56, 0 }, { D, ERPS_REQ_RAPS_NR, 57, 2 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        for (int owner = 0; owner < 2; owner++) {
            struct mock *m = mock_started(owner ? ERPS_ROLE_RPL_OWNER : ERPS_ROLE_NONE, ERPS_PORT0);
            mock_setup(m, t[i].st, false, false);
            m->e->wtr_deadline = m->e->wtb_deadline = ERPS_TIME_NEVER;
            run_row(m, mk_raps(t[i].req, ERPS_PORT1, 0x10));
            CHECK_EQ(m->e->last_row, t[i].row);
            CHECK_EQ(m->e->state, E);
            CHECK_PORTS(m, false, false);
            CHECK_EQ(m->e->wtr_deadline != ERPS_TIME_NEVER, owner && t[i].timer == 1);
            CHECK_EQ(m->e->wtb_deadline != ERPS_TIME_NEVER, owner && t[i].timer == 2);
            if (owner && t[i].timer == 2)
                CHECK(m->e->wtb_deadline == m->now + ERPS_MSEC(5500)); /* guard + 5 s */
            mock_free(m);
        }
    }
}

/* ---- Rows 30, 44: Clear in MS / FS ------------------------------------------------------------ */

TEST(row30_44_clear_command)
{
    static const struct { erps_state_t st; uint8_t row; } t[] = { { C, 30 }, { D, 44 } };
    for (size_t i = 0; i < 2; i++) {
        struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
        mock_setup(m, t[i].st, false, true);
        run_row(m, mk_req(ERPS_REQ_CLEAR, ERPS_PORT_NONE));
        CHECK_EQ(m->e->last_row, t[i].row);
        CHECK_EQ(m->e->state, E);
        CHECK_PORTS(m, false, true); /* keeps the commanded port blocked */
        CHECK_TX(m, ERPS_RAPS_NR, false, false, 1);
        CHECK(m->e->guard_deadline != ERPS_TIME_NEVER);
        CHECK(m->e->wtb_deadline != ERPS_TIME_NEVER);
        CHECK(m->e->wtr_deadline == ERPS_TIME_NEVER);
        /* no port blocked: no actions, still to E */
        mock_setup(m, t[i].st, false, false);
        m->e->guard_deadline = ERPS_TIME_NEVER;
        run_row(m, mk_req(ERPS_REQ_CLEAR, ERPS_PORT_NONE));
        CHECK_EQ(m->e->state, E);
        CHECK_EQ(m->ntx, 0);
        CHECK(m->e->guard_deadline == ERPS_TIME_NEVER);
        mock_free(m);
    }
}

/* ---- Row 36: R-APS(MS) in MS -------------------------------------------------------------- */

TEST(row36_raps_ms_in_ms)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    mock_setup(m, C, true, false);
    run_row(m, mk_raps(ERPS_REQ_RAPS_MS, ERPS_PORT1, 0x10));
    CHECK_EQ(m->e->last_row, 36);
    CHECK_EQ(m->e->state, E);
    CHECK_PORTS(m, true, false);
    CHECK_TX(m, ERPS_RAPS_NR, false, false, 0);
    CHECK(m->e->guard_deadline != ERPS_TIME_NEVER);
    /* footnote a): both ports unblocked -> stays in C, no action */
    mock_setup(m, C, false, false);
    run_row(m, mk_raps(ERPS_REQ_RAPS_MS, ERPS_PORT1, 0x10));
    CHECK_EQ(m->e->last_row, 36);
    CHECK_EQ(m->e->state, C);
    CHECK_EQ(m->ntx, 0);
    mock_free(m);
}

/* ---- Rows 58, 66, 68: owner reversion --------------------------------------------------------- */

static void revert_row(erps_req_type_t req, uint8_t row)
{
    struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT1);
    /* RPL not blocked: block, Tx(NR,RB), unblock non-RPL, flush */
    mock_setup(m, E, true, false);
    m->e->wtr_deadline = m->now + 50;
    m->e->wtb_deadline = m->now + 50;
    run_row(m, mk_req(req, ERPS_PORT_NONE));
    CHECK_EQ(m->e->last_row, row);
    CHECK_EQ(m->e->state, A);
    CHECK_PORTS(m, false, true);
    CHECK_TX(m, ERPS_RAPS_NR, true, false, 1);
    CHECK_EQ(m->flushes, 1);
    CHECK(m->e->wtr_deadline == ERPS_TIME_NEVER || req == ERPS_REQ_WTR_EXPIRES);
    CHECK(m->e->wtb_deadline == ERPS_TIME_NEVER || req == ERPS_REQ_WTB_EXPIRES);
    if (req == ERPS_REQ_WTR_EXPIRES)
        CHECK(m->e->wtb_deadline == ERPS_TIME_NEVER);
    if (req == ERPS_REQ_WTB_EXPIRES)
        CHECK(m->e->wtr_deadline == ERPS_TIME_NEVER);
    /* RPL already blocked: Tx(NR,RB,DNF), unblock non-RPL, no flush */
    mock_setup(m, E, true, true);
    run_row(m, mk_req(req, ERPS_PORT_NONE));
    CHECK_PORTS(m, false, true);
    CHECK_TX(m, ERPS_RAPS_NR, true, true, 1);
    CHECK_EQ(m->flushes, 0);
    mock_free(m);

    /* non-owner: no action, still -> A per the table */
    m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    mock_setup(m, E, true, false);
    run_row(m, mk_req(req, ERPS_PORT_NONE));
    CHECK_EQ(m->e->state, A);
    CHECK_PORTS(m, true, false);
    CHECK_EQ(m->ntx, 0);
    mock_free(m);
}

TEST(row58_66_68_owner_reversion)
{
    revert_row(ERPS_REQ_CLEAR, 58);
    revert_row(ERPS_REQ_WTR_EXPIRES, 66);
    revert_row(ERPS_REQ_WTB_EXPIRES, 68);
}

/* ---- Row 70: R-APS(NR,RB) in Pending ---------------------------------------------------------- */

TEST(row70_raps_nr_rb_pending)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    mock_setup(m, E, true, false);
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR_RB, ERPS_PORT1, 0x10));
    CHECK_EQ(m->e->last_row, 70);
    CHECK_EQ(m->e->state, A);
    CHECK_PORTS(m, false, false);
    CHECK(!m->e->tx.active);
    mock_free(m);

    m = mock_started(ERPS_ROLE_RPL_NEIGHBOUR, ERPS_PORT1);
    mock_setup(m, E, true, false);
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR_RB, ERPS_PORT0, 0x10));
    CHECK_EQ(m->e->state, A);
    CHECK_PORTS(m, false, true);
    CHECK(!m->e->tx.active);
    mock_free(m);

    m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT1);
    mock_setup(m, E, false, true);
    m->e->tx.active = true;
    m->e->wtr_deadline = m->now + 50;
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR_RB, ERPS_PORT0, 0x10));
    CHECK_EQ(m->e->state, A);
    CHECK_PORTS(m, false, true);
    CHECK(m->e->wtr_deadline == ERPS_TIME_NEVER);
    CHECK(m->e->tx.active); /* owner keeps transmitting */
    mock_free(m);
}

/* ---- Row 71: R-APS(NR) in Pending ------------------------------------------------------------ */

TEST(row71_raps_nr_pending)
{
    struct mock *m = mock_started(ERPS_ROLE_NONE, ERPS_PORT0);
    mock_setup(m, E, true, false);
    m->e->port[1].sf = true;
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR, ERPS_PORT1, 0x60)); /* higher: unblock non-failed */
    CHECK_EQ(m->e->last_row, 71);
    CHECK_EQ(m->e->state, E);
    CHECK_PORTS(m, false, false);
    CHECK(!m->e->tx.active);
    mock_setup(m, E, true, false);
    m->e->tx.active = true;
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR, ERPS_PORT1, 0x50)); /* equal (own ID): nothing */
    CHECK_PORTS(m, true, false);
    CHECK(m->e->tx.active);
    run_row(m, mk_raps(ERPS_REQ_RAPS_NR, ERPS_PORT1, 0x01)); /* lower: nothing */
    CHECK_PORTS(m, true, false);
    mock_free(m);
}

/* ---- All "No action" rows ------------------------------------------------------------------ */

TEST(no_action_rows)
{
    static const struct { erps_state_t st; erps_req_type_t req; uint8_t row; } t[] = {
        { A, ERPS_REQ_CLEAR, 2 },          { A, ERPS_REQ_WTR_EXPIRES, 10 },
        { A, ERPS_REQ_WTR_RUNNING, 11 },   { A, ERPS_REQ_WTB_EXPIRES, 12 },
        { A, ERPS_REQ_WTB_RUNNING, 13 },   { B, ERPS_REQ_CLEAR, 16 },
        { B, ERPS_REQ_RAPS_SF, 21 },       { B, ERPS_REQ_RAPS_MS, 22 },
        { B, ERPS_REQ_MS, 23 },            { B, ERPS_REQ_WTR_EXPIRES, 24 },
        { B, ERPS_REQ_WTR_RUNNING, 25 },   { B, ERPS_REQ_WTB_EXPIRES, 26 },
        { B, ERPS_REQ_WTB_RUNNING, 27 },   { C, ERPS_REQ_MS, 37 },
        { C, ERPS_REQ_WTR_EXPIRES, 38 },   { C, ERPS_REQ_WTR_RUNNING, 39 },
        { C, ERPS_REQ_WTB_EXPIRES, 40 },   { C, ERPS_REQ_WTB_RUNNING, 41 },
        { D, ERPS_REQ_RAPS_FS, 46 },       { D, ERPS_REQ_LOCAL_SF, 47 },
        { D, ERPS_REQ_RAPS_SF, 49 },       { D, ERPS_REQ_RAPS_MS, 50 },
        { D, ERPS_REQ_MS, 51 },            { D, ERPS_REQ_WTR_EXPIRES, 52 },
        { D, ERPS_REQ_WTR_RUNNING, 53 },   { D, ERPS_REQ_WTB_EXPIRES, 54 },
        { D, ERPS_REQ_WTB_RUNNING, 55 },   { E, ERPS_REQ_WTR_RUNNING, 67 },
        { E, ERPS_REQ_WTB_RUNNING, 69 },
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
        mock_setup(m, t[i].st, true, false);
        m->e->port[0].sf = true;
        erps_time_t wtr = m->e->wtr_deadline;
        bool txa = m->e->tx.active;
        erps_req_t r = (t[i].req >= ERPS_REQ_RAPS_NR && t[i].req <= ERPS_REQ_RAPS_NR_RB) ||
                               t[i].req == ERPS_REQ_RAPS_MS || t[i].req == ERPS_REQ_RAPS_SF ||
                               t[i].req == ERPS_REQ_RAPS_FS
                           ? mk_raps(t[i].req, ERPS_PORT1, 0x10)
                           : mk_req(t[i].req, ERPS_PORT0);
        r.ports = 1;
        run_row(m, r);
        CHECK_EQ(m->e->last_row, t[i].row);
        CHECK_EQ(m->e->state, t[i].st);
        CHECK_PORTS(m, true, false);
        CHECK_EQ(m->ntx, 0);
        CHECK_EQ(m->flushes, 0);
        CHECK_EQ(m->e->tx.active, txa);
        CHECK(m->e->wtr_deadline == wtr);
        CHECK(erps__fsm_row_is_noop(t[i].st, t[i].req));
        mock_free(m);
    }
}

/* ---- every row exercised at least once ---------------------------------------------------- */

TEST(all_71_rows_reachable)
{
    struct mock *m = mock_started(ERPS_ROLE_RPL_OWNER, ERPS_PORT0);
    static const erps_state_t states[5] = { A, B, C, D, E };
    for (int s = 0; s < 5; s++) {
        for (int t = ERPS_REQ_RAPS_NR; t <= ERPS_REQ_CLEAR; t++) {
            mock_setup(m, states[s], true, false);
            erps_req_t r = mk_raps((erps_req_type_t)t, ERPS_PORT1, 0x10);
            r.type = (erps_req_type_t)t;
            r.port = ERPS_PORT1;
            r.ports = 2;
            m->e->port[1].sf = t == ERPS_REQ_LOCAL_SF;
            run_row(m, r);
        }
    }
    uint8_t bm[9];
    erps_get_row_coverage(m->e, bm);
    for (int row = 1; row <= 71; row++)
        if (!(bm[row / 8] & (1u << (row % 8))))
            th_fail(__FILE__, __LINE__, "row %d never executed", row);
    mock_free(m);
}
