/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Embeddable ERPS command layer.
 *
 * A transport-agnostic command interpreter over an erps_mgr: configuration ("erps ring R1 ..."),
 * operator commands (force-switch, manual-switch, clear), and monitoring ("show erps ...") with
 * text or JSON output. Hosts embed it in any CLI framework, daemon socket or test harness:
 *
 *     erps_cli_t *cli = erps_cli_create(&platform);
 *     erps_cli_exec(cli, "erps ring R1 ring-id 1 raps-vid 100 port0 eth1 port1 eth2", out, ctx);
 *     erps_cli_exec(cli, "erps ring R1 role owner rpl-port 1", out, ctx);
 *     erps_cli_exec(cli, "erps ring R1 enable", out, ctx);
 *     erps_cli_exec(cli, "show erps ring R1 detail json", out, ctx);
 *
 * The command table (erps_cli_commands) lets a host generate its own grammar, help and
 * completion. "show running-config erps" prints the commands that recreate the configuration,
 * so a configuration file is simply a command script (erps_cli_exec_script).
 */
#ifndef ERPS_CLI_H
#define ERPS_CLI_H

#include "erps.h"
#include "erps_mgr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct erps_cli erps_cli_t;

/** Output sink: receives NUL-terminated chunks (not necessarily whole lines). */
typedef void (*erps_cli_out_fn)(void *ctx, const char *text);

/** Per-ring data the core does not need but a platform does. */
typedef struct {
    uint8_t vlans[512]; /**< protected VLAN bitmap: bit v of vlans[v/8] (VID 1..4094) */
} erps_cli_ring_extra_t;

static inline bool erps_cli_vlan_test(const erps_cli_ring_extra_t *x, unsigned vid)
{
    return vid < 4096 && (x->vlans[vid / 8] & (1u << (vid % 8)));
}

/** Platform hooks. `now` and `instance_bind` are required to create rings from the CLI. */
typedef struct {
    void *ctx;
    /** Current monotonic time (µs). Required. */
    erps_time_t (*now)(void *ctx);
    /** Resolve a port name to a handle (and optionally its MAC). 0 = found. Optional: without
     *  it, port names must be numeric handles. */
    int (*port_lookup)(void *ctx, const char *name, uint32_t *handle, uint8_t mac[6]);
    /** System MAC for "node-id auto". Optional. 0 = ok. */
    int (*system_mac)(void *ctx, uint8_t mac[6]);
    /** Bind a ring that is being enabled to the dataplane: fill @p ops and @p user. 0 = ok;
     *  on failure write a reason into @p err. */
    int (*instance_bind)(void *ctx, const erps_config_t *cfg, const erps_cli_ring_extra_t *x,
                         erps_ops_t *ops, void **user, char *err, size_t errlen);
    /** Called after a ring is disabled and its instance destroyed. Optional. */
    void (*instance_unbind)(void *ctx, void *user);
} erps_cli_platform_t;

typedef struct {
    const char *syntax; /**< e.g. "erps ring <name> force-switch port <0|1>" */
    const char *help;
} erps_cli_cmd_t;

ERPS_API erps_cli_t *erps_cli_create(const erps_cli_platform_t *platform);
/** Disables (blocking ring ports) and destroys every ring created through the CLI. */
ERPS_API void erps_cli_destroy(erps_cli_t *cli);

/** Execute one command line. Returns ERPS_OK or a negative erps_err_t; errors are also written
 *  to @p out as "% <message>". Blank lines and lines starting with '#' or '!' are ignored. */
ERPS_API erps_err_t erps_cli_exec(erps_cli_t *cli, const char *line, erps_cli_out_fn out,
                                  void *out_ctx);

/** Execute a multi-line script; stops at the first error (its 1-based line in @p err_line). */
ERPS_API erps_err_t erps_cli_exec_script(erps_cli_t *cli, const char *script,
                                         erps_cli_out_fn out, void *out_ctx, int *err_line);

/** Make an externally created instance visible to show / operator commands (not owned). */
ERPS_API erps_err_t erps_cli_attach(erps_cli_t *cli, erps_t *e);

/** The manager holding instances created through the CLI (for Rx / poll integration). */
ERPS_API erps_mgr_t *erps_cli_mgr(erps_cli_t *cli);

/** Protected VLANs of a ring (NULL if unknown). */
ERPS_API const erps_cli_ring_extra_t *erps_cli_ring_extra(const erps_cli_t *cli, const char *name);

/** Command table, for help / completion in a host CLI. */
ERPS_API const erps_cli_cmd_t *erps_cli_commands(size_t *count);

#ifdef __cplusplus
}
#endif

#endif /* ERPS_CLI_H */
