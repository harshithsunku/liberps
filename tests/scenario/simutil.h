/* SPDX-License-Identifier: Apache-2.0 */
/* Helpers shared by the scenario tests. */
#ifndef ERPS_TEST_SIMUTIL_H
#define ERPS_TEST_SIMUTIL_H

#include "erps/erps_sim.h"
#include "harness.h"

#include <stdlib.h>

/* Appendix III ring: 7 nodes A..G, RPL = link G-A, G owner, A neighbour, logical node IDs. */
enum { NA = 0, NB, NC, ND, NE, NF, NG };
static const uint8_t app3_ids[7] = { 81, 26, 89, 62, 71, 31, 75 };

static inline erps_sim_t *sim_new(unsigned nodes, unsigned owner, bool neighbour, bool revertive,
                                  const uint8_t *ids)
{
    erps_sim_cfg_t c;
    erps_sim_cfg_init(&c);
    c.nodes = nodes;
    c.owner = owner;
    c.neighbour = neighbour;
    c.node_template.revertive = revertive;
    erps_sim_t *s = erps_sim_create(&c);
    REQUIRE(s != NULL);
    if (ids)
        for (unsigned i = 0; i < nodes; i++)
            erps_sim_node_config(s, i)->node_id[5] = ids[i];
    if (getenv("ERPS_TEST_LOG"))
        erps_sim_set_log(s, stderr, (erps_log_level_t)atoi(getenv("ERPS_TEST_LOG")));
    if (getenv("ERPS_TEST_PCAP"))
        erps_sim_pcap_open(s, getenv("ERPS_TEST_PCAP"));
    REQUIRE(erps_sim_start(s) == ERPS_OK);
    return s;
}

static inline erps_sim_t *app3_new(bool revertive)
{
    return sim_new(7, NG, true, revertive, app3_ids);
}

static inline void sim_show(erps_sim_t *s)
{
    char b[512];
    erps_sim_describe(s, b, sizeof b);
    th_log("    %s\n", b);
}

/* Expect every node in state `st`, and exactly the ports in `blocked` (pairs node,port, -1
 * terminated) blocked. */
static inline int sim_expect(erps_sim_t *s, erps_state_t st, const int *blocked)
{
    int bad = 0;
    for (unsigned n = 0; n < erps_sim_nodes(s); n++) {
        if (erps_sim_node_state(s, n) != st)
            bad++;
        for (int p = 0; p < 2; p++) {
            bool want = false;
            for (const int *b = blocked; b && *b >= 0; b += 2)
                if ((unsigned)b[0] == n && b[1] == p)
                    want = true;
            if (erps_sim_port_blocked(s, n, (erps_port_t)p) != want)
                bad++;
        }
    }
    if (bad) {
        char b[512];
        erps_sim_describe(s, b, sizeof b);
        fprintf(stderr, "    ring: %s\n", b);
    }
    return bad;
}

#define EXPECT_RING(s, st, ...)                                                            \
    do {                                                                                   \
        const int exp_[] = { __VA_ARGS__, -1 };                                            \
        CHECK_EQ(sim_expect((s), (st), exp_), 0);                                          \
    } while (0)

#define CHECK_NO_LOOPS(s)                                                                  \
    do {                                                                                   \
        if (erps_sim_loop_violations(s))                                                   \
            th_fail(__FILE__, __LINE__, "loop violations: %u, first: %s",                  \
                    erps_sim_loop_violations(s), erps_sim_first_violation(s));             \
    } while (0)

#endif
