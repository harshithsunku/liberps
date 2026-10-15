/* SPDX-License-Identifier: Apache-2.0 */
/*
 * liberps ring simulator: N ERP instances wired as a ring, driven by a discrete-event virtual
 * clock. Deterministic for a given seed. Used by the scenario / chaos tests and the erps-sim
 * shell, and as a reference platform integration (see src/platform/sim/sim.c).
 *
 * Topology: link i joins node i port 1 and node (i+1) % N port 0.
 *   direction CW  = node i -> node i+1 (sent on node i port 1, received on node i+1 port 0)
 *   direction CCW = node i+1 -> node i
 *
 * Signal fail: a ring port has SSF when the direction *towards* it is down or the peer node is
 * down; it is reported to the instance after sf_detect_delay (CCM-style detection).
 *
 * Loop invariant: after every simulated instant the simulator checks that the data plane does
 * not contain a forwarding loop in either direction (every link up in that direction with both
 * of its end ports unblocked on live nodes). Violations are counted and described.
 */
#ifndef ERPS_SIM_H
#define ERPS_SIM_H

#include "erps.h"

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct erps_sim erps_sim_t;

typedef enum {
    ERPS_SIM_CW = 1,   /**< node i -> node i+1 */
    ERPS_SIM_CCW = 2,  /**< node i+1 -> node i */
    ERPS_SIM_BOTH = 3
} erps_sim_dir_t;

typedef struct {
    unsigned nodes;              /**< ring size, >= 2 */
    erps_time_t link_delay;      /**< one-way per-hop delay; default 50 us */
    erps_time_t link_jitter;     /**< extra random delay 0..jitter (FIFO preserved); default 0 */
    double loss;                 /**< per-hop frame loss probability 0..1; default 0 */
    erps_time_t sf_detect_delay; /**< SSF detection delay; default 1 ms */
    uint64_t seed;               /**< RNG seed (jitter, loss); default 1 */
    bool sw_forwarding;          /**< software R-APS forwarding in the nodes; default true */
    unsigned owner;              /**< RPL owner node; default nodes-1 (RPL = link nodes-1) */
    bool neighbour;              /**< configure the RPL neighbour (node 0); default true */
    erps_config_t node_template; /**< per-node defaults (relaxed timers etc.); node_id/role/
                                      rpl_port/ports are filled in per node */
} erps_sim_cfg_t;

/** Fill defaults: 4 nodes, revertive, spec timers, owner = node 3, neighbour = node 0. */
ERPS_API void erps_sim_cfg_init(erps_sim_cfg_t *cfg);

ERPS_API erps_sim_t *erps_sim_create(const erps_sim_cfg_t *cfg);
ERPS_API void erps_sim_destroy(erps_sim_t *sim);

/** Per-node configuration; may be changed before the node is started. */
ERPS_API erps_config_t *erps_sim_node_config(erps_sim_t *sim, unsigned node);

/** Start every node (Table 10-2 row 1) at the current simulated time. */
ERPS_API erps_err_t erps_sim_start(erps_sim_t *sim);

ERPS_API unsigned erps_sim_nodes(const erps_sim_t *sim);
ERPS_API erps_t *erps_sim_node(erps_sim_t *sim, unsigned node);
ERPS_API erps_time_t erps_sim_now(const erps_sim_t *sim);

/** Advance the virtual clock, processing every frame delivery, timer and SSF change. */
ERPS_API void erps_sim_run_until(erps_sim_t *sim, erps_time_t t);
ERPS_API void erps_sim_run_for(erps_sim_t *sim, erps_time_t dt);

/** Link control; @p link is 0..N-1. */
ERPS_API void erps_sim_link_set(erps_sim_t *sim, unsigned link, erps_sim_dir_t dir, bool up);
ERPS_API bool erps_sim_link_up(const erps_sim_t *sim, unsigned link, erps_sim_dir_t dir);

/**
 * Node failure (power off: no Rx/Tx/timers, no forwarding) and recovery (fresh restart, row 1).
 * Frames already on the wire are still delivered. Keep a node down longer than the ring's
 * R-APS propagation time (real reboots take seconds): row 1 stops the guard timer, so a node
 * restarted while its own pre-reboot messages are in flight can act on crossing stale ones.
 */
ERPS_API void erps_sim_node_set_up(erps_sim_t *sim, unsigned node, bool up);

/** Operator command on a node. */
ERPS_API erps_err_t erps_sim_command(erps_sim_t *sim, unsigned node, erps_cmd_t cmd,
                                     erps_port_t port, erps_reject_t *why);

/** Data-plane state as programmed through set_port_state(). */
ERPS_API bool erps_sim_port_blocked(const erps_sim_t *sim, unsigned node, erps_port_t port);
ERPS_API erps_state_t erps_sim_node_state(const erps_sim_t *sim, unsigned node);
ERPS_API uint64_t erps_sim_node_flushes(const erps_sim_t *sim, unsigned node);
ERPS_API uint64_t erps_sim_frames_delivered(const erps_sim_t *sim);

/** Loop invariant violations so far and a description of the first one. */
ERPS_API unsigned erps_sim_loop_violations(const erps_sim_t *sim);
ERPS_API const char *erps_sim_first_violation(const erps_sim_t *sim);

/** Simulated time of the last node-state or port-block change (for convergence checks). */
ERPS_API erps_time_t erps_sim_last_change(const erps_sim_t *sim);

/** One-line ring summary: "t=1.000000 n0:A[B-] n1:A[--] ... links: 2:x" */
ERPS_API int erps_sim_describe(const erps_sim_t *sim, char *buf, size_t len);

/** Write every transmitted frame to a pcap file (Wireshark decodes R-APS). */
ERPS_API int erps_sim_pcap_open(erps_sim_t *sim, const char *path);
ERPS_API void erps_sim_pcap_close(erps_sim_t *sim);

/** Logging from all nodes, prefixed with simulated time and node index. */
ERPS_API void erps_sim_set_log(erps_sim_t *sim, FILE *out, erps_log_level_t level);

/** Optional hook receiving every notification of every node. */
typedef void (*erps_sim_event_fn)(void *ctx, unsigned node, const erps_event_t *ev);
ERPS_API void erps_sim_set_event_hook(erps_sim_t *sim, erps_sim_event_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* ERPS_SIM_H */
