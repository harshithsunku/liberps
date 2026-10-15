/* SPDX-License-Identifier: Apache-2.0 */
/*
 * liberps — ITU-T G.8032/Y.1344 (03/2020) Ethernet Ring Protection Switching, single ring.
 *
 * One erps_t is one ERP control process (one ERP instance on one node). The library is a
 * pure, deterministic state machine: it performs no I/O, reads no clock, starts no threads and
 * keeps no global state. The host:
 *
 *   - feeds inputs: received R-APS frames, ring-port signal-fail, operator commands;
 *   - passes the current monotonic time (microseconds) with every call;
 *   - calls erps_poll() no later than erps_next_deadline();
 *   - implements erps_ops_t: transmit a frame, block/unblock a ring port, flush the FDB.
 *
 * Threading: an instance is not thread-safe; serialise all calls on one instance. Callbacks
 * must not call back into the same instance (detected, ERPS_E_BUSY). Distinct instances are
 * fully independent.
 *
 * Clause references (§) are to G.8032 (03/2020). See docs/API.md for a walkthrough.
 */
#ifndef ERPS_ERPS_H
#define ERPS_ERPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(ERPS_BUILDING) && (defined(__GNUC__) || defined(__clang__))
#define ERPS_API __attribute__((visibility("default")))
#else
#define ERPS_API
#endif

#define ERPS_VERSION_MAJOR 0
#define ERPS_VERSION_MINOR 1
#define ERPS_VERSION_PATCH 0

/** Library version string, e.g. "0.1.0". */
ERPS_API const char *erps_version(void);

/* ------------------------------------------------------------------------------------------ */
/* Basic types                                                                                */
/* ------------------------------------------------------------------------------------------ */

/** Monotonic time in microseconds. Origin is arbitrary; only differences matter. */
typedef uint64_t erps_time_t;
#define ERPS_TIME_NEVER UINT64_MAX
#define ERPS_USEC(x) ((erps_time_t)(x))
#define ERPS_MSEC(x) ((erps_time_t)(x) * 1000u)
#define ERPS_SEC(x)  ((erps_time_t)(x) * 1000000u)

typedef struct erps erps_t; /**< Opaque ERP instance. */

/** Ring port. Port 0 / port 1 correspond to "ring link 0 / 1" in the BPR field (§10.3). */
typedef enum {
    ERPS_PORT0 = 0,
    ERPS_PORT1 = 1,
    ERPS_PORT_NONE = 2
} erps_port_t;

typedef enum {
    ERPS_OK = 0,
    ERPS_E_INVAL = -1,       /**< invalid argument */
    ERPS_E_CONFIG = -2,      /**< configuration out of range or inconsistent */
    ERPS_E_STATE = -3,       /**< wrong lifecycle state (e.g. not started) */
    ERPS_E_BUSY = -4,        /**< re-entrant call from inside a callback */
    ERPS_E_NOMEM = -5,       /**< allocation failed / buffer too small for instance */
    ERPS_E_REJECTED = -6,    /**< operator command rejected; see erps_reject_t */
    ERPS_E_UNSUPPORTED = -7, /**< not supported in the configured mode */
    ERPS_E_DROPPED = -8,     /**< received frame discarded; reason counted (erps_drop_t) */
    ERPS_E_NOSPACE = -9,     /**< output buffer too small */
    ERPS_E_NOTFOUND = -10,   /**< no such object */
    ERPS_E_EXISTS = -11,     /**< object already exists */
    ERPS_E_PLATFORM = -12    /**< a platform callback reported failure */
} erps_err_t;

/** Node role on the ring (§10.1, ETH_C_MI_RAPS_RPL_Owner_Node / _Neighbour_Node). */
typedef enum {
    ERPS_ROLE_NONE = 0,
    ERPS_ROLE_RPL_OWNER,
    ERPS_ROLE_RPL_NEIGHBOUR
} erps_role_t;

/** Node state (§10.1.2, ETH_C_MI_RAPS_NodeState). Letters as in Table 10-2. */
typedef enum {
    ERPS_STATE_INIT = 0,      /**< "–": not started */
    ERPS_STATE_IDLE,          /**< A */
    ERPS_STATE_PROTECTION,    /**< B */
    ERPS_STATE_MANUAL_SWITCH, /**< C */
    ERPS_STATE_FORCED_SWITCH, /**< D */
    ERPS_STATE_PENDING,       /**< E */
    ERPS_STATE_COUNT
} erps_state_t;

/** Operator commands (§8, ETH_C_MI_RAPS_ExtCMD). */
typedef enum {
    ERPS_CMD_FORCED_SWITCH = 0,
    ERPS_CMD_MANUAL_SWITCH,
    ERPS_CMD_CLEAR
} erps_cmd_t;

/** Why a command was rejected by the local priority logic (§10.1.9, §10.2.3, §10.2.4). */
typedef enum {
    ERPS_REJECT_NONE = 0,
    ERPS_REJECT_COMPAT_V1,        /**< FS/MS filtered: compatible-version 1 (§10.1.13) */
    ERPS_REJECT_FS_ACTIVE,        /**< a local FS is already in effect (§10.2.4) */
    ERPS_REJECT_HIGHER_PRIORITY,  /**< a higher-priority request is present (§10.1.1) */
    ERPS_REJECT_MS_IN_RING,       /**< MS while an MS/FS/SF request exists in the ring (§10.2.3) */
    ERPS_REJECT_CLEAR_NOT_APPLICABLE, /**< nothing for Clear to act on (§10.1.9) */
    ERPS_REJECT_NOT_RUNNING       /**< instance not started */
} erps_reject_t;

/**
 * Top-priority request / state, in increasing priority (Table 10-1). Comparing two values
 * numerically compares their priority.
 */
typedef enum {
    ERPS_REQ_NONE = 0,
    ERPS_REQ_RAPS_NR,         /**< R-APS (NR) */
    ERPS_REQ_RAPS_NR_RB,      /**< R-APS (NR, RB) */
    ERPS_REQ_WTB_RUNNING,
    ERPS_REQ_WTB_EXPIRES,
    ERPS_REQ_WTR_RUNNING,
    ERPS_REQ_WTR_EXPIRES,
    ERPS_REQ_MS,              /**< local manual switch */
    ERPS_REQ_RAPS_MS,         /**< R-APS (MS) */
    ERPS_REQ_RAPS_SF,         /**< R-APS (SF) */
    ERPS_REQ_LOCAL_CLEAR_SF,
    ERPS_REQ_LOCAL_SF,
    ERPS_REQ_RAPS_FS,         /**< R-APS (FS) */
    ERPS_REQ_FS,              /**< local forced switch */
    ERPS_REQ_CLEAR,           /**< local clear */
    ERPS_REQ_INIT,            /**< pseudo-request: state machine initialisation (row 1) */
    ERPS_REQ_COUNT
} erps_req_type_t;

/** R-APS Request/State field values (Table 10-3). */
typedef enum {
    ERPS_RAPS_NR = 0x0,
    ERPS_RAPS_MS = 0x7,
    ERPS_RAPS_SF = 0xB,
    ERPS_RAPS_FS = 0xD,
    ERPS_RAPS_EVENT = 0xE
} erps_raps_code_t;

/** Decoded R-APS specific information (§10.3, Figure 10-3). */
typedef struct {
    uint8_t req;        /**< erps_raps_code_t (4 bits) */
    uint8_t subcode;    /**< 4 bits; 0 except for Event */
    bool rb;            /**< RPL Blocked */
    bool dnf;           /**< Do Not Flush */
    uint8_t bpr;        /**< Blocked Port Reference: 0 or 1 */
    uint8_t status;     /**< raw status octet as received/sent */
    uint8_t node_id[6]; /**< originating node (MAC) */
} erps_raps_msg_t;

/** A request as seen by the priority logic and the state machine. */
typedef struct {
    erps_req_type_t type;
    erps_port_t port;    /**< local: requested/failed port; R-APS: receiving port */
    uint8_t ports;       /**< local SF: bitmask of failed ports (bit0=port0, bit1=port1) */
    erps_raps_msg_t raps; /**< valid for ERPS_REQ_RAPS_* */
} erps_req_t;

/** Reasons a received frame is discarded (validity check, §10.1.6 and G.8013 framing). */
typedef enum {
    ERPS_DROP_NONE = 0,
    ERPS_DROP_SHORT,         /**< truncated frame or PDU */
    ERPS_DROP_DA,            /**< DA is not 01-19-A7-00-00-xx */
    ERPS_DROP_RING_ID,       /**< ring ID (DA last octet) mismatch */
    ERPS_DROP_VID,           /**< not tagged with the R-APS VID */
    ERPS_DROP_ETHERTYPE,     /**< not Ethernet OAM (0x8902) */
    ERPS_DROP_MEL,           /**< MEL differs from the ring MEL */
    ERPS_DROP_OPCODE,        /**< OAM OpCode is not R-APS (40) */
    ERPS_DROP_TLV_OFFSET,    /**< TLV offset < 32 */
    ERPS_DROP_REQ_RESERVED,  /**< reserved Request/State value */
    ERPS_DROP_EVENT_SUBCODE, /**< Event with a reserved sub-code / non-zero status */
    ERPS_DROP_NOT_RUNNING,   /**< instance not started */
    ERPS_DROP_COUNT
} erps_drop_t;

/** Message kinds used for per-type counters. */
typedef enum {
    ERPS_MSG_NR = 0,
    ERPS_MSG_NR_RB,
    ERPS_MSG_SF,
    ERPS_MSG_MS,
    ERPS_MSG_FS,
    ERPS_MSG_EVENT,
    ERPS_MSG_COUNT
} erps_msg_kind_t;

typedef enum {
    ERPS_LOG_ERR = 0,
    ERPS_LOG_WARN,
    ERPS_LOG_NOTICE,
    ERPS_LOG_INFO,
    ERPS_LOG_DEBUG,
    ERPS_LOG_TRACE
} erps_log_level_t;

/* ------------------------------------------------------------------------------------------ */
/* Configuration                                                                              */
/* ------------------------------------------------------------------------------------------ */

#define ERPS_NAME_MAX 32
#define ERPS_PORT_NAME_MAX 32

/** Ring-port binding. Opaque to the core except for display and erps_mgr demux. */
typedef struct {
    uint32_t handle;                 /**< platform port handle (e.g. ifindex) */
    char name[ERPS_PORT_NAME_MAX];   /**< display name, e.g. "eth1" */
    uint8_t mac[6];                  /**< SA for transmitted R-APS; all-zero = node_id */
} erps_port_cfg_t;

typedef struct {
    char name[ERPS_NAME_MAX];  /**< instance name used in logs / CLI, e.g. "R1" */
    uint8_t ring_id;           /**< 1..239 (§10.1.1); default 1 */
    uint16_t raps_vid;         /**< 1..4094; R-APS VLAN (§9.5) */
    uint8_t mel;               /**< 0..7; ring MEL (§10.3); default 7 */
    uint8_t node_id[6];        /**< node ID = a MAC unique to the node (§10.3 i); required */
    erps_port_cfg_t port[2];   /**< ring ports 0 and 1 */

    erps_role_t role;          /**< default ERPS_ROLE_NONE */
    erps_port_t rpl_port;      /**< RPL port for owner / neighbour */
    bool revertive;            /**< default true (§10.1.13) */
    uint8_t compat_version;    /**< 1 or 2; default 2 (§10.1.13) */

    uint32_t wtr_ms;           /**< 1..12 min in 1 min steps; default 5 min (§10.1.4) */
    uint32_t guard_ms;         /**< 10..2000 ms in 10 ms steps; default 500 (§10.1.5) */
    uint32_t holdoff_ms;       /**< 0..10000 ms in 100 ms steps; default 0 (§10.1.8) */
    bool relaxed_timers;       /**< lab use: accept any timer value >= 1 ms (and any WTR) */

    uint8_t raps_pcp;          /**< 802.1Q PCP for transmitted R-APS; default 7 */
    uint8_t tx_version;        /**< OAM Version field; ERPS_TX_VERSION_AUTO = from compat */
    uint32_t tx_interval_ms;   /**< continuous Tx period; default 5000 (§10.1.3) */
    uint32_t tx_burst_us;      /**< spacing of the initial 3-message burst; default 3333 */

    bool sw_raps_forwarding;   /**< core forwards R-APS between ring ports (software R-APS_FF) */

    bool fop_pm_enable;        /**< FOP-PM detection at the RPL owner (§10.4); default true */
    bool fop_to_enable;        /**< FOP-TO per-port Rx watchdog (§10.4); default false */
    uint32_t fop_timeout_ms;   /**< FOP-TO detection / FOP-PM clear time; default 17500 (3.5x5s) */

    uint16_t history_depth;    /**< transitions kept for erps_get_history(); <= ERPS_HISTORY_MAX */
    erps_log_level_t log_level;/**< default ERPS_LOG_NOTICE */
} erps_config_t;

#define ERPS_TX_VERSION_AUTO 0xFF
#ifndef ERPS_HISTORY_MAX
#define ERPS_HISTORY_MAX 128
#endif

/** Fill @p cfg with spec defaults. Caller must still set node_id and raps_vid. */
ERPS_API void erps_config_init(erps_config_t *cfg);

/**
 * Validate a configuration. On error, writes a human-readable reason into @p why (if non-NULL).
 */
ERPS_API erps_err_t erps_config_validate(const erps_config_t *cfg, char *why, size_t why_len);

/* ------------------------------------------------------------------------------------------ */
/* Platform interface                                                                         */
/* ------------------------------------------------------------------------------------------ */

/** block_mask bits for erps_ops_t.set_port_state(). Single ring: always both or none. */
#define ERPS_BLOCK_TRAFFIC 0x1u /**< block the instance's traffic VIDs (§9.4) */
#define ERPS_BLOCK_RAPS    0x2u /**< block forwarding of the R-APS VID (§9.5) */

typedef enum {
    ERPS_EV_STATE_CHANGE = 0, /**< node state changed */
    ERPS_EV_PORT_STATE,       /**< ring port blocked / unblocked */
    ERPS_EV_FLUSH,            /**< FDB flush performed */
    ERPS_EV_FOP_PM,           /**< failure of protocol - provisioning mismatch raised/cleared */
    ERPS_EV_FOP_TO,           /**< failure of protocol - timeout raised/cleared */
    ERPS_EV_CMD_REJECTED,     /**< operator command rejected */
    ERPS_EV_SF,               /**< local SF declared / cleared (after hold-off) */
    ERPS_EV_PLATFORM_ERROR,   /**< a platform callback failed */
    ERPS_EV_COUNT
} erps_event_type_t;

typedef struct {
    erps_event_type_t type;
    erps_time_t time;
    union {
        struct { erps_state_t from, to; uint8_t row; erps_req_type_t req; } state;
        struct { erps_port_t port; bool blocked; } port;
        struct { bool raised; erps_port_t port; uint8_t node_id[6]; } fop;
        struct { erps_cmd_t cmd; erps_port_t port; erps_reject_t reason; } reject;
        struct { erps_port_t port; bool sf; } sf;
        struct { const char *op; int rc; erps_port_t port; } platform;
    } u;
} erps_event_t;

/**
 * Platform callbacks. tx, set_port_state and flush_fdb are required. Return 0 on success,
 * non-zero on failure. A failed set_port_state() is retried from erps_poll() until it succeeds.
 */
typedef struct {
    /** Transmit a complete Ethernet frame (DA .. End TLV/padding, no FCS) on a ring port. */
    int (*tx)(void *user, erps_port_t port, const uint8_t *frame, size_t len);
    /** Apply blocking to a ring port for this instance (ERPS_BLOCK_* bits; 0 = forwarding). */
    int (*set_port_state)(void *user, erps_port_t port, unsigned block_mask);
    /** Flush FDB entries learned on the ring ports for this instance's VLANs (§9.6). */
    int (*flush_fdb)(void *user);
    /** Optional: notifications for alarms / management. */
    void (*notify)(void *user, const erps_event_t *ev);
    /** Optional: log sink. @p msg has no trailing newline. */
    void (*log)(void *user, erps_log_level_t level, const char *msg);
} erps_ops_t;

/* ------------------------------------------------------------------------------------------ */
/* Lifecycle                                                                                  */
/* ------------------------------------------------------------------------------------------ */

/** Allocate and initialise an instance (not yet started). */
ERPS_API erps_err_t erps_create(const erps_config_t *cfg, const erps_ops_t *ops, void *user,
                                erps_t **out);

/** Size of an instance, for erps_init_static(). */
ERPS_API size_t erps_instance_size(void);

/** Initialise an instance in caller-provided memory (no malloc; e.g. embedded NOS). */
ERPS_API erps_err_t erps_init_static(void *mem, size_t mem_len, const erps_config_t *cfg,
                                     const erps_ops_t *ops, void *user, erps_t **out);

/** Release an instance created by erps_create(). Does not touch the dataplane. */
ERPS_API void erps_destroy(erps_t *e);

/** Run state machine initialisation (Table 10-2 row 1). Ports are programmed, Tx starts. */
ERPS_API erps_err_t erps_start(erps_t *e, erps_time_t now);

/**
 * Stop the control process: stop Tx and timers, state -> INIT. If @p block_ports, both ring
 * ports are blocked first (safe default: never leaves an unprotected loop behind).
 */
ERPS_API erps_err_t erps_stop(erps_t *e, erps_time_t now, bool block_ports);

/** Change timers at run time (validated). Running timers keep their current deadline. */
ERPS_API erps_err_t erps_set_timers(erps_t *e, uint32_t wtr_ms, uint32_t guard_ms,
                                    uint32_t holdoff_ms);

/** Change revertive mode at run time (ignored in compatible-version 1, which forces revertive). */
ERPS_API erps_err_t erps_set_revertive(erps_t *e, bool revertive);

ERPS_API void erps_set_log_level(erps_t *e, erps_log_level_t level);
ERPS_API const erps_config_t *erps_get_config(const erps_t *e);
ERPS_API void *erps_get_user(const erps_t *e);

/* ------------------------------------------------------------------------------------------ */
/* Inputs                                                                                     */
/* ------------------------------------------------------------------------------------------ */

/**
 * Deliver a received frame from ring port @p port: DA, SA, 802.1Q tag with the R-APS VID,
 * EtherType 0x8902, OAM PDU. Returns ERPS_OK if accepted as an R-APS for this instance
 * (even if the guard timer then discards it), ERPS_E_DROPPED if discarded by validation.
 */
ERPS_API erps_err_t erps_rx_frame(erps_t *e, erps_port_t port, const uint8_t *frame, size_t len,
                                  erps_time_t now);

/** Metadata for erps_rx_pdu(), for platforms that strip the Ethernet header / VLAN tag. */
typedef struct {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t vid;
    uint8_t pcp;
} erps_rx_meta_t;

/** As erps_rx_frame() but with the Ethernet header already parsed. @p pdu starts at MEL/Version. */
ERPS_API erps_err_t erps_rx_pdu(erps_t *e, erps_port_t port, const erps_rx_meta_t *meta,
                                const uint8_t *pdu, size_t len, erps_time_t now);

/**
 * Ring-port server signal fail (ETH_CI_SSF, §10.1.7). Call on every change (repeats are
 * harmless). SF is declared after the hold-off time if still present (§10.1.8).
 */
ERPS_API erps_err_t erps_set_signal_fail(erps_t *e, erps_port_t port, bool ssf, erps_time_t now);

/**
 * Operator command (§8). @p port is required for FS/MS and ignored for Clear.
 * Returns ERPS_E_REJECTED (with @p reason if non-NULL) when the local priority logic refuses it.
 */
ERPS_API erps_err_t erps_command(erps_t *e, erps_cmd_t cmd, erps_port_t port, erps_time_t now,
                                 erps_reject_t *reason);

/** Process due timers and transmissions. Call at or after erps_next_deadline(). */
ERPS_API erps_err_t erps_poll(erps_t *e, erps_time_t now);

/** Earliest time erps_poll() must be called, or ERPS_TIME_NEVER. */
ERPS_API erps_time_t erps_next_deadline(const erps_t *e);

/** Transmit a burst of three R-APS Event (flush) messages (§10.1.3). Diagnostic / interop aid. */
ERPS_API erps_err_t erps_send_flush_event(erps_t *e, erps_time_t now);

/* ------------------------------------------------------------------------------------------ */
/* Introspection and debugging                                                                */
/* ------------------------------------------------------------------------------------------ */

typedef struct {
    bool blocked;         /**< ETH_C_MI_RAPS_PortState */
    bool ssf;             /**< raw signal fail input */
    bool sf;              /**< SF declared (after hold-off) */
    bool holdoff_running;
    bool fop_to;          /**< FOP-TO raised on this port */
    bool rx_seen;         /**< at least one valid R-APS received */
    erps_time_t last_rx_time;
    erps_raps_msg_t last_rx;
    bool flush_pair_valid; /**< flush logic (node ID, BPR) pair stored (§10.1.10) */
    uint8_t flush_node[6];
    uint8_t flush_bpr;
} erps_port_status_t;

typedef struct {
    erps_state_t state;
    bool running;
    erps_port_status_t port[2];
    erps_req_t top;           /**< last top-priority request signalled to the state machine */
    uint8_t last_row;         /**< last Table 10-2 row executed (0 = none) */
    erps_time_t last_change;  /**< time of last state change */
    bool fs_active;           /**< local FS latched */
    bool ms_active;           /**< local MS latched */
    erps_port_t cmd_port;     /**< port of the latched FS / MS */
    bool tx_active;           /**< continuous R-APS transmission running */
    erps_raps_msg_t tx_msg;   /**< message being transmitted */
    erps_time_t wtr_remaining, wtb_remaining, guard_remaining; /**< 0 if not running */
    bool wtr_running, wtb_running, guard_running;
    bool fop_pm;              /**< FOP-PM raised */
    bool revertive;           /**< effective (compat-version 1 forces true) */
} erps_status_t;

typedef struct {
    struct {
        uint64_t rx_frames;                   /**< frames handed to the instance */
        uint64_t rx_raps[ERPS_MSG_COUNT];     /**< valid R-APS by kind */
        uint64_t tx_raps[ERPS_MSG_COUNT];
        uint64_t rx_drop[ERPS_DROP_COUNT];
        uint64_t sw_forwarded;                /**< forwarded to the other port (software R-APS_FF) */
        uint64_t blocked, unblocked;          /**< port state transitions */
    } port[2];
    uint64_t rx_own;           /**< own node ID received: ignored by priority logic */
    uint64_t rx_guard;         /**< discarded while the guard timer ran (§10.1.5) */
    uint64_t rx_suppressed;    /**< lost to a persistent local request (no FSM run) */
    uint64_t fsm_runs;
    uint64_t state_changes;
    uint64_t flushes;          /**< flush_fdb() calls */
    uint64_t flush_requests;   /**< flush triggers before coalescing */
    uint64_t commands;
    uint64_t commands_rejected;
    uint64_t fop_pm, fop_to;   /**< raise events */
    uint64_t platform_errors;
    uint64_t clock_backwards;  /**< calls with a time earlier than a previous call */
} erps_counters_t;

/** Actions recorded in the transition history (Table 10-2 action list, effective ones). */
typedef enum {
    ERPS_ACT_NONE = 0,
    ERPS_ACT_BLOCK_P0, ERPS_ACT_BLOCK_P1, ERPS_ACT_UNBLOCK_P0, ERPS_ACT_UNBLOCK_P1,
    ERPS_ACT_TX, ERPS_ACT_TX_SAME, ERPS_ACT_STOP_TX, ERPS_ACT_FLUSH,
    ERPS_ACT_START_WTR, ERPS_ACT_STOP_WTR, ERPS_ACT_START_WTB, ERPS_ACT_STOP_WTB,
    ERPS_ACT_START_GUARD, ERPS_ACT_STOP_GUARD,
    ERPS_ACT_COUNT
} erps_action_t;

#define ERPS_TRACE_MAX_ACTIONS 12

/** One state machine run (Table 10-2 row execution). */
typedef struct {
    uint32_t seq;
    erps_time_t time;
    erps_req_t req;            /**< top-priority request that triggered the row */
    erps_state_t from, to;
    uint8_t row;               /**< Table 10-2 row number */
    uint8_t ports_before;      /**< bit0 = port0 blocked, bit1 = port1 blocked */
    uint8_t ports_after;
    uint8_t n_actions;
    uint8_t actions[ERPS_TRACE_MAX_ACTIONS]; /**< erps_action_t, in execution order */
    erps_raps_msg_t tx;        /**< message started by ERPS_ACT_TX */
    uint32_t repeats;          /**< further identical no-action runs folded into this entry */
    erps_time_t last_time;     /**< time of the last folded repeat */
} erps_trace_t;

ERPS_API erps_err_t erps_get_status(const erps_t *e, erps_status_t *st);
ERPS_API erps_err_t erps_get_counters(const erps_t *e, erps_counters_t *c);
ERPS_API void erps_clear_counters(erps_t *e);

/** Copy up to @p max history entries, oldest first. Returns the number copied. */
ERPS_API size_t erps_get_history(const erps_t *e, erps_trace_t *out, size_t max);

/** Table 10-2 rows executed since creation: bit r of @p bitmap[r/8] for row r (1..71). */
ERPS_API void erps_get_row_coverage(const erps_t *e, uint8_t bitmap[9]);

typedef enum { ERPS_FMT_TEXT = 0, ERPS_FMT_JSON } erps_fmt_t;

#define ERPS_DUMP_CONFIG   0x1u
#define ERPS_DUMP_STATUS   0x2u
#define ERPS_DUMP_COUNTERS 0x4u
#define ERPS_DUMP_HISTORY  0x8u
#define ERPS_DUMP_ALL      0xFu

/**
 * Render instance information. snprintf semantics: returns the length that the full output
 * needs (excluding NUL); output is truncated (and NUL-terminated) if @p len is too small.
 */
ERPS_API int erps_dump(const erps_t *e, unsigned what, erps_fmt_t fmt, char *buf, size_t len);

/** Render one history entry as a single text line. Returns snprintf-style length. */
ERPS_API int erps_trace_format(const erps_trace_t *t, char *buf, size_t len);

/* ------------------------------------------------------------------------------------------ */
/* Names                                                                                      */
/* ------------------------------------------------------------------------------------------ */

ERPS_API const char *erps_strerror(erps_err_t err);
ERPS_API const char *erps_state_str(erps_state_t s);     /**< "Idle", "Protection", ... */
ERPS_API char erps_state_letter(erps_state_t s);         /**< 'A'..'E', '-' */
ERPS_API const char *erps_role_str(erps_role_t r);
ERPS_API const char *erps_req_type_str(erps_req_type_t t);
ERPS_API const char *erps_cmd_str(erps_cmd_t c);
ERPS_API const char *erps_reject_str(erps_reject_t r);
ERPS_API const char *erps_drop_str(erps_drop_t d);
ERPS_API const char *erps_msg_kind_str(erps_msg_kind_t k);
ERPS_API const char *erps_action_str(erps_action_t a);
ERPS_API const char *erps_event_str(erps_event_type_t t);
ERPS_API const char *erps_log_level_str(erps_log_level_t l);

/** "R-APS(SF,DNF) 00:11:22:33:44:55 bpr1" style rendering. Returns snprintf-style length. */
ERPS_API int erps_raps_msg_format(const erps_raps_msg_t *m, char *buf, size_t len);
/** Render a request incl. port / R-APS details. Returns snprintf-style length. */
ERPS_API int erps_req_format(const erps_req_t *r, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ERPS_ERPS_H */
