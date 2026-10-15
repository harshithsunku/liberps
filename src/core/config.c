/* SPDX-License-Identifier: Apache-2.0 */
/* Configuration defaults and validation. Ranges are those of G.8032 §10.1.4/5/8 and §10.1.13. */
#include "erps_internal.h"

#include <stdio.h>
#include <string.h>

void erps_config_init(erps_config_t *c)
{
    if (!c)
        return;
    memset(c, 0, sizeof *c);
    snprintf(c->name, sizeof c->name, "erps");
    c->ring_id = 1;
    c->raps_vid = 0; /* must be set by the caller */
    c->mel = 7;
    for (int p = 0; p < 2; p++)
        c->port[p].handle = (uint32_t)p;
    c->role = ERPS_ROLE_NONE;
    c->rpl_port = ERPS_PORT0;
    c->revertive = true;
    c->compat_version = 2;
    c->wtr_ms = 5u * 60u * 1000u;
    c->guard_ms = 500;
    c->holdoff_ms = 0;
    c->relaxed_timers = false;
    c->raps_pcp = 7;
    c->tx_version = ERPS_TX_VERSION_AUTO;
    c->tx_interval_ms = 5000;
    c->tx_burst_us = 3333;
    c->sw_raps_forwarding = false;
    c->fop_pm_enable = true;
    c->fop_to_enable = false;
    c->fop_timeout_ms = 17500;
    c->history_depth = ERPS_HISTORY_MAX;
    c->log_level = ERPS_LOG_NOTICE;
}

#define FAIL(...)                                                                            \
    do {                                                                                     \
        if (why && why_len)                                                                  \
            snprintf(why, why_len, __VA_ARGS__);                                             \
        return ERPS_E_CONFIG;                                                                \
    } while (0)

#define MAX_RELAXED_MS (24u * 3600u * 1000u)

erps_err_t erps_config_validate(const erps_config_t *c, char *why, size_t why_len)
{
    static const uint8_t zero[6];
    if (why && why_len)
        why[0] = '\0';
    if (!c)
        return ERPS_E_INVAL;
    if (!memchr(c->name, '\0', sizeof c->name))
        FAIL("name is not NUL-terminated within %d octets", ERPS_NAME_MAX);
    for (int p = 0; p < 2; p++)
        if (!memchr(c->port[p].name, '\0', sizeof c->port[p].name))
            FAIL("port %d name is not NUL-terminated", p);
    if (c->ring_id < 1 || c->ring_id > 239)
        FAIL("ring-id %u out of range 1..239", c->ring_id);
    if (c->raps_vid < 1 || c->raps_vid > 4094)
        FAIL("R-APS VID %u out of range 1..4094", c->raps_vid);
    if (c->mel > 7)
        FAIL("MEL %u out of range 0..7", c->mel);
    if (memcmp(c->node_id, zero, 6) == 0)
        FAIL("node-id must be set (a unicast MAC unique to the node)");
    if (c->node_id[0] & 1u)
        FAIL("node-id must be a unicast MAC");
    if (c->role != ERPS_ROLE_NONE && c->role != ERPS_ROLE_RPL_OWNER &&
        c->role != ERPS_ROLE_RPL_NEIGHBOUR)
        FAIL("invalid role %d", (int)c->role);
    if (c->role != ERPS_ROLE_NONE && c->rpl_port != ERPS_PORT0 && c->rpl_port != ERPS_PORT1)
        FAIL("rpl-port must be 0 or 1 for an RPL owner / neighbour");
    if (c->compat_version != 1 && c->compat_version != 2)
        FAIL("compatible-version must be 1 or 2");

    if (c->relaxed_timers) {
        if (c->wtr_ms < 1 || c->wtr_ms > MAX_RELAXED_MS)
            FAIL("WTR %u ms out of range 1..%u ms", c->wtr_ms, MAX_RELAXED_MS);
        if (c->guard_ms < 1 || c->guard_ms > MAX_RELAXED_MS)
            FAIL("guard %u ms out of range 1..%u ms", c->guard_ms, MAX_RELAXED_MS);
        if (c->holdoff_ms > MAX_RELAXED_MS)
            FAIL("hold-off %u ms out of range 0..%u ms", c->holdoff_ms, MAX_RELAXED_MS);
    } else {
        if (c->wtr_ms < 60000u || c->wtr_ms > 720000u || c->wtr_ms % 60000u)
            FAIL("WTR %u ms: must be 1..12 min in 1 min steps (G.8032 10.1.4)", c->wtr_ms);
        if (c->guard_ms < 10u || c->guard_ms > 2000u || c->guard_ms % 10u)
            FAIL("guard %u ms: must be 10..2000 ms in 10 ms steps (G.8032 10.1.5)", c->guard_ms);
        if (c->holdoff_ms > 10000u || c->holdoff_ms % 100u)
            FAIL("hold-off %u ms: must be 0..10 s in 100 ms steps (G.8032 10.1.8)",
                 c->holdoff_ms);
    }

    if (c->raps_pcp > 7)
        FAIL("R-APS PCP %u out of range 0..7", c->raps_pcp);
    if (c->tx_version != ERPS_TX_VERSION_AUTO && c->tx_version > 31)
        FAIL("tx version %u out of range 0..31", c->tx_version);
    if (c->tx_interval_ms < 1 || c->tx_interval_ms > 60000u)
        FAIL("tx interval %u ms out of range 1..60000", c->tx_interval_ms);
    if (c->tx_burst_us < 1 || c->tx_burst_us > 1000000u)
        FAIL("tx burst spacing %u us out of range 1..1000000", c->tx_burst_us);
    if (c->fop_timeout_ms < 1 || c->fop_timeout_ms > MAX_RELAXED_MS)
        FAIL("FOP timeout %u ms out of range", c->fop_timeout_ms);
    if (c->history_depth > ERPS_HISTORY_MAX)
        FAIL("history depth %u exceeds %d", c->history_depth, ERPS_HISTORY_MAX);
    if (c->log_level > ERPS_LOG_TRACE)
        FAIL("invalid log level %d", (int)c->log_level);
    return ERPS_OK;
}
