/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Debuggability: transition history (one entry per Table 10-2 row executed), status snapshot,
 * counters, and text / JSON rendering used by the CLI layer ("show erps ...").
 */
#include "erps_internal.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------ */
/* History                                                                                    */
/* ------------------------------------------------------------------------------------------ */

static uint32_t depth(const erps_t *e)
{
    return e->cfg.history_depth ? e->cfg.history_depth : 1u;
}

void erps__trace_begin(erps_t *e, const erps_req_t *req, uint8_t row)
{
    erps_trace_t *t = &e->hist[e->hist_next % depth(e)];
    memset(t, 0, sizeof *t);
    t->seq = ++e->seq;
    t->time = e->now;
    t->req = *req;
    t->from = e->state;
    t->row = row;
    t->ports_before = erps__ports_mask(e);
    e->cur = t;
}

void erps__trace_action(erps_t *e, erps_action_t a)
{
    erps_trace_t *t = e->cur;
    if (t && t->n_actions < ERPS_TRACE_MAX_ACTIONS)
        t->actions[t->n_actions++] = (uint8_t)a;
}

void erps__trace_tx(erps_t *e, const erps_raps_msg_t *m)
{
    if (e->cur)
        e->cur->tx = *m;
}

static bool is_noop(const erps_trace_t *t)
{
    return t->n_actions == 0 && t->from == t->to && t->ports_before == t->ports_after;
}

void erps__trace_end(erps_t *e)
{
    erps_trace_t *t = e->cur;
    if (!t)
        return;
    t->to = e->state;
    t->ports_after = erps__ports_mask(e);
    e->cur = NULL;
    /* Periodic R-APS re-runs no-action rows (e.g. row 21 every 5 s in Protection). Fold such a
     * run into the previous entry when it is the same no-op, so history keeps the transitions. */
    if (is_noop(t) && e->hist_count) {
        erps_trace_t *prev = &e->hist[(e->hist_next + depth(e) - 1u) % depth(e)];
        if (is_noop(prev) && prev->row == t->row && prev->req.type == t->req.type) {
            prev->repeats++;
            prev->last_time = t->time;
            e->seq--;
            return;
        }
    }
    e->hist_next = (e->hist_next + 1u) % depth(e);
    if (e->hist_count < depth(e))
        e->hist_count++;
    if (e->ops.log && e->cfg.log_level >= ERPS_LOG_DEBUG) {
        char line[256];
        erps_trace_format(t, line, sizeof line);
        erps__log(e, ERPS_LOG_DEBUG, "%s", line);
    }
}

size_t erps_get_history(const erps_t *e, erps_trace_t *out, size_t max)
{
    if (!e || e->magic != ERPS_MAGIC || !out)
        return 0;
    uint32_t d = depth(e);
    uint32_t n = e->hist_count;
    if (max < n)
        n = (uint32_t)max;
    /* oldest of the last n entries */
    uint32_t start = (e->hist_next + d - n) % d;
    for (uint32_t i = 0; i < n; i++)
        out[i] = e->hist[(start + i) % d];
    return n;
}

void erps_get_row_coverage(const erps_t *e, uint8_t bitmap[9])
{
    if (!bitmap)
        return;
    if (!e || e->magic != ERPS_MAGIC) {
        memset(bitmap, 0, 9);
        return;
    }
    memcpy(bitmap, e->rows_hit, 9);
}

/* ------------------------------------------------------------------------------------------ */
/* Snapshots                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static erps_time_t remaining(const erps_t *e, erps_time_t deadline)
{
    if (deadline == ERPS_TIME_NEVER)
        return 0;
    return deadline > e->now ? deadline - e->now : 0;
}

erps_err_t erps_get_status(const erps_t *e, erps_status_t *st)
{
    if (!e || e->magic != ERPS_MAGIC || !st)
        return ERPS_E_INVAL;
    memset(st, 0, sizeof *st);
    st->state = e->state;
    st->running = e->running;
    for (int p = 0; p < 2; p++) {
        const struct erps_port *pp = &e->port[p];
        erps_port_status_t *ps = &st->port[p];
        ps->blocked = pp->blocked;
        ps->ssf = pp->ssf;
        ps->sf = pp->sf;
        ps->holdoff_running = pp->holdoff_deadline != ERPS_TIME_NEVER;
        ps->fop_to = pp->fop_to;
        ps->rx_seen = pp->rx_seen;
        ps->last_rx_time = pp->last_rx_time;
        ps->last_rx = pp->last_rx;
        ps->flush_pair_valid = pp->pair_valid;
        memcpy(ps->flush_node, pp->pair_node, 6);
        ps->flush_bpr = pp->pair_bpr;
    }
    st->top = e->top;
    st->last_row = e->last_row;
    st->last_change = e->last_change;
    st->fs_active = e->fs_active;
    st->ms_active = e->ms_active;
    st->cmd_port = (e->fs_active || e->ms_active) ? e->cmd_port : ERPS_PORT_NONE;
    st->tx_active = e->tx.active;
    st->tx_msg = e->tx.msg;
    st->wtr_running = e->wtr_deadline != ERPS_TIME_NEVER;
    st->wtb_running = e->wtb_deadline != ERPS_TIME_NEVER;
    st->guard_running = e->guard_deadline != ERPS_TIME_NEVER;
    st->wtr_remaining = remaining(e, e->wtr_deadline);
    st->wtb_remaining = remaining(e, e->wtb_deadline);
    st->guard_remaining = remaining(e, e->guard_deadline);
    st->fop_pm = e->fop_pm;
    st->revertive = erps__revertive(e);
    return ERPS_OK;
}

erps_err_t erps_get_counters(const erps_t *e, erps_counters_t *c)
{
    if (!e || e->magic != ERPS_MAGIC || !c)
        return ERPS_E_INVAL;
    *c = e->ctr;
    return ERPS_OK;
}

void erps_clear_counters(erps_t *e)
{
    if (e && e->magic == ERPS_MAGIC)
        memset(&e->ctr, 0, sizeof e->ctr);
}

/* ------------------------------------------------------------------------------------------ */
/* Rendering                                                                                  */
/* ------------------------------------------------------------------------------------------ */

struct out {
    char *buf;
    size_t len;
    size_t pos; /* total length required so far */
};

__attribute__((format(printf, 2, 3))) static void outf(struct out *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *dst = o->pos < o->len ? o->buf + o->pos : NULL;
    size_t room = o->pos < o->len ? o->len - o->pos : 0;
    int n = vsnprintf(dst, room, fmt, ap);
    va_end(ap);
    if (n > 0)
        o->pos += (size_t)n;
}

static void fmt_time(erps_time_t t, char *b, size_t n)
{
    snprintf(b, n, "%llu.%06llu", (unsigned long long)(t / 1000000u),
             (unsigned long long)(t % 1000000u));
}

static void fmt_dur(erps_time_t us, char *b, size_t n)
{
    unsigned long long ms = (unsigned long long)(us / 1000u);
    if (ms >= 60000u)
        snprintf(b, n, "%llum%02llus", ms / 60000u, (ms / 1000u) % 60u);
    else if (ms >= 1000u)
        snprintf(b, n, "%llu.%03llus", ms / 1000u, ms % 1000u);
    else
        snprintf(b, n, "%llums", ms);
}

static void ports_str(uint8_t mask, char b[3])
{
    b[0] = (mask & 1u) ? 'B' : '-';
    b[1] = (mask & 2u) ? 'B' : '-';
    b[2] = '\0';
}

int erps_trace_format(const erps_trace_t *t, char *buf, size_t len)
{
    struct out o = { buf, len, 0 };
    if (len)
        buf[0] = '\0';
    if (!t)
        return 0;
    char ts[32], req[112], pb[3], pa[3];
    fmt_time(t->time, ts, sizeof ts);
    erps_req_format(&t->req, req, sizeof req);
    ports_str(t->ports_before, pb);
    ports_str(t->ports_after, pa);
    outf(&o, "#%u t=%s %c->%c row %u %s [", t->seq, ts, erps_state_letter(t->from),
         erps_state_letter(t->to), t->row, req);
    for (unsigned i = 0; i < t->n_actions; i++)
        outf(&o, "%s%s", i ? " " : "", erps_action_str((erps_action_t)t->actions[i]));
    outf(&o, "] ports %s->%s", pb, pa);
    if (t->repeats) {
        char lt[32];
        fmt_time(t->last_time, lt, sizeof lt);
        outf(&o, " (x%u more, last t=%s)", t->repeats, lt);
    }
    for (unsigned i = 0; i < t->n_actions; i++) {
        if (t->actions[i] == ERPS_ACT_TX) {
            char m[96];
            erps_raps_msg_format(&t->tx, m, sizeof m);
            outf(&o, " tx=%s", m);
            break;
        }
    }
    return (int)o.pos;
}

static void text_config(struct out *o, const erps_t *e)
{
    const erps_config_t *c = &e->cfg;
    char mac[24];
    erps__fmt_mac(c->node_id, mac, sizeof mac);
    outf(o, "ERP instance %s\n", c->name);
    outf(o, "  Ring ID %u, R-APS VID %u, MEL %u, node ID %s\n", c->ring_id, c->raps_vid, c->mel,
         mac);
    if (c->role == ERPS_ROLE_NONE)
        outf(o, "  Role: none\n");
    else
        outf(o, "  Role: %s, RPL port %d (%s)\n", erps_role_str(c->role), (int)c->rpl_port,
             c->port[c->rpl_port].name[0] ? c->port[c->rpl_port].name : "-");
    outf(o, "  Ports: 0=%s 1=%s\n", c->port[0].name[0] ? c->port[0].name : "-",
         c->port[1].name[0] ? c->port[1].name : "-");
    outf(o, "  Mode: %s, compatible-version %u%s\n",
         erps__revertive(e) ? "revertive" : "non-revertive", c->compat_version,
         c->sw_raps_forwarding ? ", software R-APS forwarding" : "");
    char wtr[24], guard[24], ho[24], wtb[24];
    fmt_dur(ERPS_MSEC(c->wtr_ms), wtr, sizeof wtr);
    fmt_dur(ERPS_MSEC(c->guard_ms), guard, sizeof guard);
    fmt_dur(ERPS_MSEC(c->holdoff_ms), ho, sizeof ho);
    fmt_dur(ERPS_MSEC((uint64_t)c->guard_ms + ERPS_WTB_EXTRA_MS), wtb, sizeof wtb);
    outf(o, "  Timers: WTR %s, WTB %s, guard %s, hold-off %s\n", wtr, wtb, guard, ho);
}

static void text_status(struct out *o, const erps_t *e)
{
    erps_status_t st;
    erps_get_status(e, &st);
    char t[32], b[112];
    fmt_time(st.last_change, t, sizeof t);
    outf(o, "  State: %s (%c)%s, since t=%s, last row %u\n", erps_state_str(st.state),
         erps_state_letter(st.state), st.running ? "" : " [stopped]", t, st.last_row);
    erps_req_format(&st.top, b, sizeof b);
    outf(o, "  Top request: %s\n", b);
    for (int p = 0; p < 2; p++) {
        const erps_port_status_t *ps = &st.port[p];
        outf(o, "  Port %d %-10s %-9s SF:%s%s", p, e->cfg.port[p].name[0] ? e->cfg.port[p].name : "-",
             ps->blocked ? "Blocked" : "Forwarding", ps->sf ? "yes" : "no",
             ps->holdoff_running ? " (hold-off)" : "");
        if (ps->rx_seen) {
            erps_raps_msg_format(&ps->last_rx, b, sizeof b);
            char ago[24];
            fmt_dur(e->now > ps->last_rx_time ? e->now - ps->last_rx_time : 0, ago, sizeof ago);
            outf(o, "  last rx %s (%s ago)", b, ago);
        }
        if (ps->fop_to)
            outf(o, "  FOP-TO");
        outf(o, "\n");
    }
    if (st.tx_active) {
        erps_raps_msg_format(&st.tx_msg, b, sizeof b);
        outf(o, "  Tx: %s\n", b);
    } else {
        outf(o, "  Tx: none\n");
    }
    char d[24];
    outf(o, "  Timers running:");
    if (st.wtr_running) {
        fmt_dur(st.wtr_remaining, d, sizeof d);
        outf(o, " WTR(%s left)", d);
    }
    if (st.wtb_running) {
        fmt_dur(st.wtb_remaining, d, sizeof d);
        outf(o, " WTB(%s left)", d);
    }
    if (st.guard_running) {
        fmt_dur(st.guard_remaining, d, sizeof d);
        outf(o, " guard(%s left)", d);
    }
    if (!st.wtr_running && !st.wtb_running && !st.guard_running)
        outf(o, " none");
    outf(o, "\n");
    if (st.fs_active || st.ms_active)
        outf(o, "  Command: %s on port %d\n", st.fs_active ? "forced-switch" : "manual-switch",
             (int)st.cmd_port);
    outf(o, "  Alarms: FOP-PM %s, FOP-TO p0 %s p1 %s\n", st.fop_pm ? "RAISED" : "no",
         st.port[0].fop_to ? "RAISED" : "no", st.port[1].fop_to ? "RAISED" : "no");
}

static void text_counters(struct out *o, const erps_t *e)
{
    const erps_counters_t *c = &e->ctr;
    outf(o, "  Counters:            port0      port1\n");
    for (int k = 0; k < ERPS_MSG_COUNT; k++)
        outf(o, "    rx %-8s %10llu %10llu\n", erps_msg_kind_str((erps_msg_kind_t)k),
             (unsigned long long)c->port[0].rx_raps[k], (unsigned long long)c->port[1].rx_raps[k]);
    for (int k = 0; k < ERPS_MSG_COUNT; k++)
        outf(o, "    tx %-8s %10llu %10llu\n", erps_msg_kind_str((erps_msg_kind_t)k),
             (unsigned long long)c->port[0].tx_raps[k], (unsigned long long)c->port[1].tx_raps[k]);
    for (int k = 1; k < ERPS_DROP_COUNT; k++) {
        if (c->port[0].rx_drop[k] || c->port[1].rx_drop[k])
            outf(o, "    drop %-18s %10llu %10llu\n", erps_drop_str((erps_drop_t)k),
                 (unsigned long long)c->port[0].rx_drop[k],
                 (unsigned long long)c->port[1].rx_drop[k]);
    }
    outf(o, "    forwarded          %10llu %10llu\n", (unsigned long long)c->port[0].sw_forwarded,
         (unsigned long long)c->port[1].sw_forwarded);
    outf(o, "    blocked/unblocked  %5llu/%-4llu %5llu/%-4llu\n",
         (unsigned long long)c->port[0].blocked, (unsigned long long)c->port[0].unblocked,
         (unsigned long long)c->port[1].blocked, (unsigned long long)c->port[1].unblocked);
    outf(o,
         "    fsm-runs %llu, state-changes %llu, flushes %llu (requests %llu), rx own %llu, "
         "guard %llu, suppressed %llu\n",
         (unsigned long long)c->fsm_runs, (unsigned long long)c->state_changes,
         (unsigned long long)c->flushes, (unsigned long long)c->flush_requests,
         (unsigned long long)c->rx_own, (unsigned long long)c->rx_guard,
         (unsigned long long)c->rx_suppressed);
    outf(o,
         "    commands %llu (rejected %llu), FOP-PM %llu, FOP-TO %llu, platform errors %llu, "
         "clock-backwards %llu\n",
         (unsigned long long)c->commands, (unsigned long long)c->commands_rejected,
         (unsigned long long)c->fop_pm, (unsigned long long)c->fop_to,
         (unsigned long long)c->platform_errors, (unsigned long long)c->clock_backwards);
}

static void text_history(struct out *o, const erps_t *e)
{
    erps_trace_t h[ERPS_HISTORY_MAX];
    size_t n = erps_get_history(e, h, ERPS_HISTORY_MAX);
    outf(o, "  History (%zu):\n", n);
    for (size_t i = 0; i < n; i++) {
        char line[256];
        erps_trace_format(&h[i], line, sizeof line);
        outf(o, "    %s\n", line);
    }
}

/* ---- JSON -------------------------------------------------------------------------------- */

static void json_str(struct out *o, const char *s)
{
    outf(o, "\"");
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            outf(o, "\\%c", ch);
        else if (ch < 0x20)
            outf(o, "\\u%04x", ch);
        else
            outf(o, "%c", ch);
    }
    outf(o, "\"");
}

static void json_mac(struct out *o, const uint8_t m[6])
{
    char b[24];
    erps__fmt_mac(m, b, sizeof b);
    json_str(o, b);
}

static void json_msg(struct out *o, const erps_raps_msg_t *m)
{
    char b[96];
    erps_raps_msg_format(m, b, sizeof b);
    outf(o, "{\"text\":");
    json_str(o, b);
    outf(o, ",\"req\":%u,\"rb\":%s,\"dnf\":%s,\"bpr\":%u,\"node_id\":", m->req,
         m->rb ? "true" : "false", m->dnf ? "true" : "false", m->bpr);
    json_mac(o, m->node_id);
    outf(o, "}");
}

static void json_config(struct out *o, const erps_t *e)
{
    const erps_config_t *c = &e->cfg;
    outf(o, "\"config\":{\"name\":");
    json_str(o, c->name);
    outf(o, ",\"ring_id\":%u,\"raps_vid\":%u,\"mel\":%u,\"node_id\":", c->ring_id, c->raps_vid,
         c->mel);
    json_mac(o, c->node_id);
    outf(o, ",\"role\":");
    json_str(o, erps_role_str(c->role));
    outf(o, ",\"rpl_port\":%d,\"ports\":[", (int)c->rpl_port);
    for (int p = 0; p < 2; p++) {
        outf(o, "%s{\"name\":", p ? "," : "");
        json_str(o, c->port[p].name);
        outf(o, ",\"handle\":%u}", c->port[p].handle);
    }
    outf(o,
         "],\"revertive\":%s,\"compat_version\":%u,\"wtr_ms\":%u,\"guard_ms\":%u,"
         "\"wtb_ms\":%u,\"holdoff_ms\":%u,\"sw_raps_forwarding\":%s}",
         erps__revertive(e) ? "true" : "false", c->compat_version, c->wtr_ms, c->guard_ms,
         c->guard_ms + ERPS_WTB_EXTRA_MS, c->holdoff_ms, c->sw_raps_forwarding ? "true" : "false");
}

static void json_status(struct out *o, const erps_t *e)
{
    erps_status_t st;
    erps_get_status(e, &st);
    char b[112];
    outf(o, "\"status\":{\"state\":");
    json_str(o, erps_state_str(st.state));
    outf(o, ",\"state_letter\":\"%c\",\"running\":%s,\"last_row\":%u,\"last_change_us\":%llu,",
         erps_state_letter(st.state), st.running ? "true" : "false", st.last_row,
         (unsigned long long)st.last_change);
    erps_req_format(&st.top, b, sizeof b);
    outf(o, "\"top_request\":");
    json_str(o, b);
    outf(o, ",\"ports\":[");
    for (int p = 0; p < 2; p++) {
        const erps_port_status_t *ps = &st.port[p];
        outf(o, "%s{\"blocked\":%s,\"sf\":%s,\"ssf\":%s,\"holdoff\":%s,\"fop_to\":%s", p ? "," : "",
             ps->blocked ? "true" : "false", ps->sf ? "true" : "false",
             ps->ssf ? "true" : "false", ps->holdoff_running ? "true" : "false",
             ps->fop_to ? "true" : "false");
        if (ps->rx_seen) {
            outf(o, ",\"last_rx_us\":%llu,\"last_rx\":", (unsigned long long)ps->last_rx_time);
            json_msg(o, &ps->last_rx);
        }
        outf(o, "}");
    }
    outf(o, "],\"tx\":");
    if (st.tx_active)
        json_msg(o, &st.tx_msg);
    else
        outf(o, "null");
    outf(o,
         ",\"timers\":{\"wtr_us\":%llu,\"wtb_us\":%llu,\"guard_us\":%llu},"
         "\"command\":",
         (unsigned long long)st.wtr_remaining, (unsigned long long)st.wtb_remaining,
         (unsigned long long)st.guard_remaining);
    if (st.fs_active || st.ms_active)
        outf(o, "{\"type\":\"%s\",\"port\":%d}", st.fs_active ? "forced-switch" : "manual-switch",
             (int)st.cmd_port);
    else
        outf(o, "null");
    outf(o, ",\"fop_pm\":%s}", st.fop_pm ? "true" : "false");
}

static void json_counters(struct out *o, const erps_t *e)
{
    const erps_counters_t *c = &e->ctr;
    outf(o, "\"counters\":{\"ports\":[");
    for (int p = 0; p < 2; p++) {
        outf(o, "%s{\"rx_frames\":%llu,\"rx\":{", p ? "," : "",
             (unsigned long long)c->port[p].rx_frames);
        for (int k = 0; k < ERPS_MSG_COUNT; k++)
            outf(o, "%s\"%s\":%llu", k ? "," : "", erps_msg_kind_str((erps_msg_kind_t)k),
                 (unsigned long long)c->port[p].rx_raps[k]);
        outf(o, "},\"tx\":{");
        for (int k = 0; k < ERPS_MSG_COUNT; k++)
            outf(o, "%s\"%s\":%llu", k ? "," : "", erps_msg_kind_str((erps_msg_kind_t)k),
                 (unsigned long long)c->port[p].tx_raps[k]);
        outf(o, "},\"drop\":{");
        for (int k = 1; k < ERPS_DROP_COUNT; k++)
            outf(o, "%s\"%s\":%llu", k > 1 ? "," : "", erps_drop_str((erps_drop_t)k),
                 (unsigned long long)c->port[p].rx_drop[k]);
        outf(o, "},\"forwarded\":%llu,\"blocked\":%llu,\"unblocked\":%llu}",
             (unsigned long long)c->port[p].sw_forwarded, (unsigned long long)c->port[p].blocked,
             (unsigned long long)c->port[p].unblocked);
    }
    outf(o,
         "],\"rx_own\":%llu,\"rx_guard\":%llu,\"rx_suppressed\":%llu,\"fsm_runs\":%llu,"
         "\"state_changes\":%llu,\"flushes\":%llu,\"flush_requests\":%llu,\"commands\":%llu,"
         "\"commands_rejected\":%llu,\"fop_pm\":%llu,\"fop_to\":%llu,\"platform_errors\":%llu,"
         "\"clock_backwards\":%llu}",
         (unsigned long long)c->rx_own, (unsigned long long)c->rx_guard,
         (unsigned long long)c->rx_suppressed, (unsigned long long)c->fsm_runs,
         (unsigned long long)c->state_changes, (unsigned long long)c->flushes,
         (unsigned long long)c->flush_requests, (unsigned long long)c->commands,
         (unsigned long long)c->commands_rejected, (unsigned long long)c->fop_pm,
         (unsigned long long)c->fop_to, (unsigned long long)c->platform_errors,
         (unsigned long long)c->clock_backwards);
}

static void json_history(struct out *o, const erps_t *e)
{
    erps_trace_t h[ERPS_HISTORY_MAX];
    size_t n = erps_get_history(e, h, ERPS_HISTORY_MAX);
    outf(o, "\"history\":[");
    for (size_t i = 0; i < n; i++) {
        const erps_trace_t *t = &h[i];
        char req[112];
        erps_req_format(&t->req, req, sizeof req);
        outf(o, "%s{\"seq\":%u,\"time_us\":%llu,\"from\":\"%c\",\"to\":\"%c\",\"row\":%u,\"req\":",
             i ? "," : "", t->seq, (unsigned long long)t->time, erps_state_letter(t->from),
             erps_state_letter(t->to), t->row);
        json_str(o, req);
        outf(o, ",\"actions\":[");
        for (unsigned a = 0; a < t->n_actions; a++)
            outf(o, "%s\"%s\"", a ? "," : "", erps_action_str((erps_action_t)t->actions[a]));
        outf(o, "],\"ports_before\":%u,\"ports_after\":%u,\"repeats\":%u}", t->ports_before,
             t->ports_after, t->repeats);
    }
    outf(o, "]");
}

int erps_dump(const erps_t *e, unsigned what, erps_fmt_t fmt, char *buf, size_t len)
{
    struct out o = { buf, len, 0 };
    if (len)
        buf[0] = '\0';
    if (!e || e->magic != ERPS_MAGIC)
        return 0;
    if (fmt == ERPS_FMT_JSON) {
        bool first = true;
        outf(&o, "{");
#define SECTION(flag, fn)                                                                    \
    if (what & (flag)) {                                                                     \
        if (!first)                                                                          \
            outf(&o, ",");                                                                   \
        first = false;                                                                       \
        fn(&o, e);                                                                           \
    }
        SECTION(ERPS_DUMP_CONFIG, json_config)
        SECTION(ERPS_DUMP_STATUS, json_status)
        SECTION(ERPS_DUMP_COUNTERS, json_counters)
        SECTION(ERPS_DUMP_HISTORY, json_history)
#undef SECTION
        outf(&o, "}");
    } else {
        if (what & ERPS_DUMP_CONFIG)
            text_config(&o, e);
        else
            outf(&o, "ERP instance %s\n", e->cfg.name);
        if (what & ERPS_DUMP_STATUS)
            text_status(&o, e);
        if (what & ERPS_DUMP_COUNTERS)
            text_counters(&o, e);
        if (what & ERPS_DUMP_HISTORY)
            text_history(&o, e);
    }
    return (int)o.pos;
}
