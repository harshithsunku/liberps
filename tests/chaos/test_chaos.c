/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Randomised protocol stress (property test) on the simulator.
 *
 * For each seed: a ring of 2..16 nodes (random owner, with or without neighbour, revertive or
 * not, random link delay / jitter / loss) receives a random sequence of link failures (bi- and
 * unidirectional), node failures, FS / MS / Clear commands and time advances. Properties:
 *
 *   P1  no forwarding loop ever exists (checked by the simulator after every instant);
 *   P2  after all faults and commands are removed and the ring is given time (and, if
 *       non-revertive, the owner a Clear), every node is Idle and exactly the RPL end(s) are
 *       blocked (checked for per-hop loss <= 10 %; heavier loss only slows convergence);
 *   P3  the instances never report a platform error or run time backwards.
 *
 * Env: ERPS_CHAOS_SEEDS (default 300), ERPS_CHAOS_SEED0 (default 1), ERPS_CHAOS_VERBOSE.
 * A failure prints the seed; re-run it alone with ERPS_CHAOS_SEED0=<seed> ERPS_CHAOS_SEEDS=1
 * ERPS_CHAOS_VERBOSE=1 to see the event script and the simulator log.
 */
#include "erps/erps_sim.h"
#include "harness.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static uint64_t rs;

static uint64_t rnd(void)
{
    rs ^= rs >> 12;
    rs ^= rs << 25;
    rs ^= rs >> 27;
    return rs * 0x2545F4914F6CDD1DULL;
}

static unsigned below(unsigned n)
{
    return (unsigned)(rnd() % n);
}

static unsigned long env_ul(const char *name, unsigned long dflt)
{
    const char *v = getenv(name);
    return v ? strtoul(v, NULL, 0) : dflt;
}

struct run {
    unsigned nodes, owner;
    bool neighbour, revertive;
    double loss;
    erps_time_t delay, jitter;
    bool verbose;
};

__attribute__((format(printf, 3, 4))) static void say(const struct run *r, erps_sim_t *s,
                                                      const char *fmt, ...)
{
    if (!r->verbose)
        return;
    char d[512], m[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof m, fmt, ap);
    va_end(ap);
    erps_sim_describe(s, d, sizeof d);
    fprintf(stderr, "      %-32s | %s\n", m, d);
}

static bool check_final(erps_sim_t *s, const struct run *r, char *why, size_t len)
{
    unsigned neigh = (r->owner + 1) % r->nodes;
    for (unsigned n = 0; n < r->nodes; n++) {
        if (erps_sim_node_state(s, n) != ERPS_STATE_IDLE) {
            snprintf(why, len, "node %u not Idle", n);
            return false;
        }
        for (int p = 0; p < 2; p++) {
            bool want = (n == r->owner && p == 1) || (r->neighbour && n == neigh && p == 0);
            if (erps_sim_port_blocked(s, n, (erps_port_t)p) != want) {
                snprintf(why, len, "node %u port %d %s", n, p, want ? "not blocked" : "blocked");
                return false;
            }
        }
    }
    return true;
}

static int one_seed(uint64_t seed, bool verbose)
{
    rs = seed * 0x9E3779B97F4A7C15ULL + 1;
    struct run r;
    memset(&r, 0, sizeof r);
    r.verbose = verbose;
    r.nodes = 2 + below(15);
    r.owner = below(r.nodes);
    r.neighbour = below(3) != 0;
    r.revertive = below(4) != 0;
    r.delay = 10 + below(500);
    r.jitter = below(3) == 0 ? below(200) : 0;
    r.loss = below(3) == 0 ? (double)below(30) / 100.0 : 0.0;

    erps_sim_cfg_t c;
    erps_sim_cfg_init(&c);
    c.nodes = r.nodes;
    c.owner = r.owner;
    c.neighbour = r.neighbour;
    c.link_delay = r.delay;
    c.link_jitter = r.jitter;
    c.loss = r.loss;
    c.seed = seed;
    c.node_template.revertive = r.revertive;
    c.node_template.holdoff_ms = below(4) == 0 ? 100u * below(5) : 0;
    erps_sim_t *s = erps_sim_create(&c);
    if (!s)
        return -1;
    /* random distinct node IDs, so ID-ordering decisions vary */
    for (unsigned i = 0; i < r.nodes; i++) {
        uint8_t *id = erps_sim_node_config(s, i)->node_id;
        id[3] = (uint8_t)below(256);
        id[4] = (uint8_t)i;
    }
    if (verbose) {
        erps_sim_set_log(s, stderr, ERPS_LOG_DEBUG);
        fprintf(stderr, "    seed %llu: %u nodes owner %u %s %s delay %lluus jitter %lluus loss %.2f\n",
                (unsigned long long)seed, r.nodes, r.owner, r.neighbour ? "neighbour" : "no-neighbour",
                r.revertive ? "revertive" : "non-revertive", (unsigned long long)r.delay,
                (unsigned long long)r.jitter, r.loss);
    }
    if (erps_sim_start(s) != ERPS_OK) {
        erps_sim_destroy(s);
        return -1;
    }
    erps_sim_run_for(s, ERPS_SEC(below(2) ? 301 : 2));

    erps_time_t down_since[16];
    for (unsigned i = 0; i < 16; i++)
        down_since[i] = ERPS_TIME_NEVER;
    unsigned steps = 5 + below(40);
    for (unsigned i = 0; i < steps; i++) {
        unsigned what = below(100), n = below(r.nodes);
        if (what < 30) {
            static const erps_sim_dir_t dirs[] = { ERPS_SIM_BOTH, ERPS_SIM_BOTH, ERPS_SIM_CW, ERPS_SIM_CCW };
            erps_sim_dir_t d = dirs[below(4)];
            bool up = below(2) == 0;
            erps_sim_link_set(s, n, d, up);
            say(&r, s, "link %s %u dir %u", up ? "up" : "down", n, (unsigned)d);
        } else if (what < 40) {
            bool up = below(2) == 0;
            /* A node cannot reboot faster than its own in-flight R-APS drain: row 1 stops the
             * guard timer, so a sub-millisecond power cycle could act on crossing stale
             * messages. Real restarts take seconds; model at least 2 s down. */
            if (up && down_since[n] != ERPS_TIME_NEVER &&
                erps_sim_now(s) < down_since[n] + ERPS_SEC(2))
                erps_sim_run_until(s, down_since[n] + ERPS_SEC(2));
            if (!up && down_since[n] == ERPS_TIME_NEVER)
                down_since[n] = erps_sim_now(s);
            if (up)
                down_since[n] = ERPS_TIME_NEVER;
            erps_sim_node_set_up(s, n, up);
            say(&r, s, "node %s %u", up ? "up" : "down", n);
        } else if (what < 55) {
            erps_port_t p = below(2) ? ERPS_PORT1 : ERPS_PORT0;
            erps_cmd_t cmd = below(3) == 0 ? ERPS_CMD_FORCED_SWITCH : ERPS_CMD_MANUAL_SWITCH;
            erps_sim_command(s, n, cmd, p, NULL);
            say(&r, s, "%s node %u port %u", erps_cmd_str(cmd), n, (unsigned)p);
        } else if (what < 65) {
            erps_sim_command(s, n, ERPS_CMD_CLEAR, ERPS_PORT_NONE, NULL);
            say(&r, s, "clear node %u", n);
        } else {
            /* advance: mostly short (inside guard / WTB windows), sometimes long */
            static const erps_time_t dt[] = { 0, 200, 1000, 3333, 20000, 300000, 600000,
                                              2000000, 6000000, 30000000, 400000000 };
            erps_time_t d = dt[below((unsigned)(sizeof dt / sizeof dt[0]))];
            erps_sim_run_for(s, d);
            say(&r, s, "run %llu us", (unsigned long long)d);
        }
    }

    /* heal everything (respecting the minimum reboot time) */
    erps_sim_run_for(s, ERPS_SEC(2));
    for (unsigned l = 0; l < r.nodes; l++)
        erps_sim_link_set(s, l, ERPS_SIM_BOTH, true);
    for (unsigned n = 0; n < r.nodes; n++)
        erps_sim_node_set_up(s, n, true);
    erps_sim_run_for(s, ERPS_SEC(1));
    for (unsigned n = 0; n < r.nodes; n++) {
        erps_status_t st;
        erps_get_status(erps_sim_node(s, n), &st);
        if (st.fs_active || st.ms_active)
            erps_sim_command(s, n, ERPS_CMD_CLEAR, ERPS_PORT_NONE, NULL);
    }
    /* enough for guard, WTB, a full WTR and lossy retransmission */
    erps_sim_run_for(s, ERPS_SEC(700));
    if (!r.revertive) {
        erps_sim_command(s, r.owner, ERPS_CMD_CLEAR, ERPS_PORT_NONE, NULL);
        /* (NR,RB) every 5 s must cross up to 15 hops at up to 30 % loss each */
        erps_sim_run_for(s, ERPS_SEC(700));
    }

    int bad = 0;
    char why[256] = "";
    if (erps_sim_loop_violations(s)) {
        snprintf(why, sizeof why, "loop: %s", erps_sim_first_violation(s));
        bad = 1;
    } else if (r.loss <= 0.10 && !check_final(s, &r, why, sizeof why)) {
        /* P2 only under realistic loss: convergence time grows like 1/(1-loss)^hops (one-way
         * (NR,RB) propagation with an RPL neighbour), heavy-loss seeds only check P1 and P3 */
        bad = 2;
    } else {
        for (unsigned n = 0; n < r.nodes; n++) {
            erps_counters_t ct;
            erps_get_counters(erps_sim_node(s, n), &ct);
            if (ct.platform_errors || ct.clock_backwards) {
                snprintf(why, sizeof why, "node %u platform errors / clock", n);
                bad = 3;
            }
        }
    }
    if (bad) {
        char d[512];
        erps_sim_describe(s, d, sizeof d);
        fprintf(stderr,
                "    seed %llu FAILED (%s): %u nodes owner %u %s %s loss %.2f\n      %s\n"
                "      rerun: ERPS_CHAOS_SEED0=%llu ERPS_CHAOS_SEEDS=1 ERPS_CHAOS_VERBOSE=1 "
                "build/gcc/test-chaos\n",
                (unsigned long long)seed, why, r.nodes, r.owner,
                r.neighbour ? "neighbour" : "no-neighbour", r.revertive ? "revertive" : "non-revertive",
                r.loss, d, (unsigned long long)seed);
    }
    erps_sim_destroy(s);
    return bad;
}

TEST(chaos_random_faults_and_commands)
{
    unsigned long seeds = env_ul("ERPS_CHAOS_SEEDS", 300);
    unsigned long seed0 = env_ul("ERPS_CHAOS_SEED0", 1);
    bool verbose = env_ul("ERPS_CHAOS_VERBOSE", 0) != 0;
    unsigned failed = 0;
    for (unsigned long i = 0; i < seeds; i++) {
        int rc = one_seed(seed0 + i, verbose);
        if (rc)
            failed++;
        if (failed >= 5)
            break;
    }
    th_log("    %lu seeds, %u failed\n", seeds, failed);
    CHECK_EQ(failed, 0);
}
