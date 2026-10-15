/* SPDX-License-Identifier: Apache-2.0 */
/* Enum names and small formatting helpers. */
#include "erps_internal.h"

#include <stdio.h>

#define STR_(x) #x
#define STR(x) STR_(x)

const char *erps_version(void)
{
    return STR(ERPS_VERSION_MAJOR) "." STR(ERPS_VERSION_MINOR) "." STR(ERPS_VERSION_PATCH);
}

const char *erps_strerror(erps_err_t err)
{
    switch (err) {
    case ERPS_OK: return "ok";
    case ERPS_E_INVAL: return "invalid argument";
    case ERPS_E_CONFIG: return "invalid configuration";
    case ERPS_E_STATE: return "wrong state";
    case ERPS_E_BUSY: return "re-entrant call";
    case ERPS_E_NOMEM: return "out of memory";
    case ERPS_E_REJECTED: return "rejected";
    case ERPS_E_UNSUPPORTED: return "unsupported";
    case ERPS_E_DROPPED: return "frame dropped";
    case ERPS_E_NOSPACE: return "buffer too small";
    case ERPS_E_NOTFOUND: return "not found";
    case ERPS_E_EXISTS: return "already exists";
    case ERPS_E_PLATFORM: return "platform error";
    }
    return "unknown error";
}

const char *erps_state_str(erps_state_t s)
{
    switch (s) {
    case ERPS_STATE_INIT: return "Init";
    case ERPS_STATE_IDLE: return "Idle";
    case ERPS_STATE_PROTECTION: return "Protection";
    case ERPS_STATE_MANUAL_SWITCH: return "Manual-switch";
    case ERPS_STATE_FORCED_SWITCH: return "Forced-switch";
    case ERPS_STATE_PENDING: return "Pending";
    case ERPS_STATE_COUNT: break;
    }
    return "?";
}

char erps_state_letter(erps_state_t s)
{
    switch (s) {
    case ERPS_STATE_IDLE: return 'A';
    case ERPS_STATE_PROTECTION: return 'B';
    case ERPS_STATE_MANUAL_SWITCH: return 'C';
    case ERPS_STATE_FORCED_SWITCH: return 'D';
    case ERPS_STATE_PENDING: return 'E';
    default: return '-';
    }
}

const char *erps_role_str(erps_role_t r)
{
    switch (r) {
    case ERPS_ROLE_NONE: return "none";
    case ERPS_ROLE_RPL_OWNER: return "rpl-owner";
    case ERPS_ROLE_RPL_NEIGHBOUR: return "rpl-neighbour";
    }
    return "?";
}

const char *erps_req_type_str(erps_req_type_t t)
{
    switch (t) {
    case ERPS_REQ_NONE: return "none";
    case ERPS_REQ_RAPS_NR: return "R-APS(NR)";
    case ERPS_REQ_RAPS_NR_RB: return "R-APS(NR,RB)";
    case ERPS_REQ_WTB_RUNNING: return "WTB-running";
    case ERPS_REQ_WTB_EXPIRES: return "WTB-expires";
    case ERPS_REQ_WTR_RUNNING: return "WTR-running";
    case ERPS_REQ_WTR_EXPIRES: return "WTR-expires";
    case ERPS_REQ_MS: return "MS";
    case ERPS_REQ_RAPS_MS: return "R-APS(MS)";
    case ERPS_REQ_RAPS_SF: return "R-APS(SF)";
    case ERPS_REQ_LOCAL_CLEAR_SF: return "local-clear-SF";
    case ERPS_REQ_LOCAL_SF: return "local-SF";
    case ERPS_REQ_RAPS_FS: return "R-APS(FS)";
    case ERPS_REQ_FS: return "FS";
    case ERPS_REQ_CLEAR: return "Clear";
    case ERPS_REQ_INIT: return "init";
    case ERPS_REQ_COUNT: break;
    }
    return "?";
}

const char *erps_cmd_str(erps_cmd_t c)
{
    switch (c) {
    case ERPS_CMD_FORCED_SWITCH: return "forced-switch";
    case ERPS_CMD_MANUAL_SWITCH: return "manual-switch";
    case ERPS_CMD_CLEAR: return "clear";
    }
    return "?";
}

const char *erps_reject_str(erps_reject_t r)
{
    switch (r) {
    case ERPS_REJECT_NONE: return "none";
    case ERPS_REJECT_COMPAT_V1: return "FS/MS not available in compatible-version 1";
    case ERPS_REJECT_FS_ACTIVE: return "a local forced switch is already active";
    case ERPS_REJECT_HIGHER_PRIORITY: return "a higher-priority local request is active";
    case ERPS_REJECT_MS_IN_RING: return "an MS, FS or SF request already exists in the ring";
    case ERPS_REJECT_CLEAR_NOT_APPLICABLE: return "nothing to clear on this node";
    case ERPS_REJECT_NOT_RUNNING: return "instance not running";
    }
    return "?";
}

const char *erps_drop_str(erps_drop_t d)
{
    switch (d) {
    case ERPS_DROP_NONE: return "none";
    case ERPS_DROP_SHORT: return "truncated";
    case ERPS_DROP_DA: return "bad-da";
    case ERPS_DROP_RING_ID: return "ring-id-mismatch";
    case ERPS_DROP_VID: return "vid-mismatch";
    case ERPS_DROP_ETHERTYPE: return "not-oam";
    case ERPS_DROP_MEL: return "mel-mismatch";
    case ERPS_DROP_OPCODE: return "not-raps";
    case ERPS_DROP_TLV_OFFSET: return "bad-tlv-offset";
    case ERPS_DROP_REQ_RESERVED: return "reserved-request";
    case ERPS_DROP_EVENT_SUBCODE: return "reserved-event";
    case ERPS_DROP_NOT_RUNNING: return "not-running";
    case ERPS_DROP_COUNT: break;
    }
    return "?";
}

const char *erps_msg_kind_str(erps_msg_kind_t k)
{
    switch (k) {
    case ERPS_MSG_NR: return "NR";
    case ERPS_MSG_NR_RB: return "NR-RB";
    case ERPS_MSG_SF: return "SF";
    case ERPS_MSG_MS: return "MS";
    case ERPS_MSG_FS: return "FS";
    case ERPS_MSG_EVENT: return "Event";
    case ERPS_MSG_COUNT: break;
    }
    return "?";
}

const char *erps_action_str(erps_action_t a)
{
    switch (a) {
    case ERPS_ACT_NONE: return "none";
    case ERPS_ACT_BLOCK_P0: return "block-p0";
    case ERPS_ACT_BLOCK_P1: return "block-p1";
    case ERPS_ACT_UNBLOCK_P0: return "unblock-p0";
    case ERPS_ACT_UNBLOCK_P1: return "unblock-p1";
    case ERPS_ACT_TX: return "tx";
    case ERPS_ACT_TX_SAME: return "tx(same)";
    case ERPS_ACT_STOP_TX: return "stop-tx";
    case ERPS_ACT_FLUSH: return "flush";
    case ERPS_ACT_START_WTR: return "start-wtr";
    case ERPS_ACT_STOP_WTR: return "stop-wtr";
    case ERPS_ACT_START_WTB: return "start-wtb";
    case ERPS_ACT_STOP_WTB: return "stop-wtb";
    case ERPS_ACT_START_GUARD: return "start-guard";
    case ERPS_ACT_STOP_GUARD: return "stop-guard";
    case ERPS_ACT_COUNT: break;
    }
    return "?";
}

const char *erps_event_str(erps_event_type_t t)
{
    switch (t) {
    case ERPS_EV_STATE_CHANGE: return "state-change";
    case ERPS_EV_PORT_STATE: return "port-state";
    case ERPS_EV_FLUSH: return "flush";
    case ERPS_EV_FOP_PM: return "fop-pm";
    case ERPS_EV_FOP_TO: return "fop-to";
    case ERPS_EV_CMD_REJECTED: return "command-rejected";
    case ERPS_EV_SF: return "signal-fail";
    case ERPS_EV_PLATFORM_ERROR: return "platform-error";
    case ERPS_EV_COUNT: break;
    }
    return "?";
}

const char *erps_log_level_str(erps_log_level_t l)
{
    switch (l) {
    case ERPS_LOG_ERR: return "error";
    case ERPS_LOG_WARN: return "warning";
    case ERPS_LOG_NOTICE: return "notice";
    case ERPS_LOG_INFO: return "info";
    case ERPS_LOG_DEBUG: return "debug";
    case ERPS_LOG_TRACE: return "trace";
    }
    return "?";
}

int erps__fmt_mac(const uint8_t m[6], char *buf, size_t len)
{
    return snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4],
                    m[5]);
}

static const char *raps_code_str(uint8_t req)
{
    switch (req) {
    case ERPS_RAPS_NR: return "NR";
    case ERPS_RAPS_MS: return "MS";
    case ERPS_RAPS_SF: return "SF";
    case ERPS_RAPS_FS: return "FS";
    case ERPS_RAPS_EVENT: return "Event";
    default: return "reserved";
    }
}

int erps_raps_msg_format(const erps_raps_msg_t *m, char *buf, size_t len)
{
    char mac[24];
    if (!m)
        return snprintf(buf, len, "(null)");
    erps__fmt_mac(m->node_id, mac, sizeof mac);
    return snprintf(buf, len, "R-APS(%s%s%s) %s bpr%u", raps_code_str(m->req), m->rb ? ",RB" : "",
                    m->dnf ? ",DNF" : "", mac, m->bpr);
}

int erps_req_format(const erps_req_t *r, char *buf, size_t len)
{
    if (!r)
        return snprintf(buf, len, "(null)");
    switch (r->type) {
    case ERPS_REQ_RAPS_NR:
    case ERPS_REQ_RAPS_NR_RB:
    case ERPS_REQ_RAPS_SF:
    case ERPS_REQ_RAPS_MS:
    case ERPS_REQ_RAPS_FS: {
        char m[80];
        erps_raps_msg_format(&r->raps, m, sizeof m);
        return snprintf(buf, len, "%s @p%d", m, (int)r->port);
    }
    case ERPS_REQ_LOCAL_SF:
        return snprintf(buf, len, "local-SF p%s%s", (r->ports & 1u) ? "0" : "",
                        (r->ports & 2u) ? ((r->ports & 1u) ? ",1" : "1") : "");
    case ERPS_REQ_FS:
    case ERPS_REQ_MS:
    case ERPS_REQ_LOCAL_CLEAR_SF:
        return snprintf(buf, len, "%s p%d", erps_req_type_str(r->type), (int)r->port);
    default:
        return snprintf(buf, len, "%s", erps_req_type_str(r->type));
    }
}
