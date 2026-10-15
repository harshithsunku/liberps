/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ERPS command layer — see include/erps/erps_cli.h.
 *
 * Grammar (tokens separated by blanks; "<x>" = argument):
 *
 *   erps ring <name> [ring-id <1-239>] [raps-vid <1-4094>] [mel <0-7>] [port0 <port>]
 *                    [port1 <port>] [node-id <mac>|auto] [role owner|neighbour|none]
 *                    [rpl-port <0|1>] [revertive|non-revertive] [compat-version <1|2>]
 *                    [timers] [wtr <dur>] [guard <dur>] [holdoff <dur>] [protected-vlans <list>]
 *                    [pcp <0-7>] [sw-forwarding on|off] [fop-pm on|off] [fop-to on|off]
 *                    [fop-timeout <dur>] [relaxed-timers on|off]
 *   erps ring <name> enable | disable
 *   no erps ring <name>
 *   erps ring <name> force-switch port <0|1> | manual-switch port <0|1> | clear | flush-event
 *   show erps [summary] [json]
 *   show erps ring <name> [detail|status|config|counters|history] [json]
 *   show running-config erps
 *   clear erps [ring <name>] counters
 *   debug erps [ring <name>] level <error|warning|notice|info|debug|trace>
 *   help
 *
 * Durations: 5m, 300s, 500ms, 1h; a bare number is milliseconds.
 */
#include "erps/erps_cli.h"
#include "erps/erps_pdu.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 48
#define MAX_LINE 2048

struct ring {
    erps_config_t cfg;
    erps_cli_ring_extra_t x;
    bool node_id_explicit;
    bool enabled;
    bool external; /* attached instance, not created through the CLI */
    erps_t *e;
    void *user;
};

struct erps_cli {
    erps_cli_platform_t plat;
    erps_mgr_t *mgr;
    struct ring *rings;
    size_t n, cap;
};

struct out {
    erps_cli_out_fn fn;
    void *ctx;
};

static const erps_cli_cmd_t commands[] = {
    { "erps ring <name> ring-id <1-239> raps-vid <1-4094> port0 <port> port1 <port>",
      "create / configure a ring instance (keywords may be combined on one line)" },
    { "erps ring <name> role owner|neighbour|none [rpl-port <0|1>]", "RPL role of this node" },
    { "erps ring <name> node-id <mac>|auto", "node ID (default: system MAC)" },
    { "erps ring <name> mel <0-7>", "ring MEG level (default 7)" },
    { "erps ring <name> revertive|non-revertive", "reversion mode (default revertive)" },
    { "erps ring <name> compat-version <1|2>", "G.8032v1 interworking (default 2)" },
    { "erps ring <name> timers [wtr <dur>] [guard <dur>] [holdoff <dur>]",
      "WTR 1-12 min, guard 10ms-2s, hold-off 0-10s" },
    { "erps ring <name> protected-vlans [add|remove] <list>",
      "VLANs protected by the instance, e.g. 10-20,30" },
    { "erps ring <name> init-timer wtr|wtb",
      "owner timer at start-up: wtr (Table 10-2) or wtb (App. VIII, faster cold start)" },
    { "erps ring <name> pcp <0-7>", "802.1Q priority of transmitted R-APS" },
    { "erps ring <name> sw-forwarding on|off", "forward R-APS between ring ports in software" },
    { "erps ring <name> fop-pm on|off | fop-to on|off | fop-timeout <dur>",
      "failure-of-protocol alarms" },
    { "erps ring <name> relaxed-timers on|off", "allow out-of-range timer values (lab use)" },
    { "erps ring <name> enable|disable", "start / stop the instance (disable blocks ring ports)" },
    { "no erps ring <name>", "delete a ring" },
    { "erps ring <name> force-switch port <0|1>", "operator forced switch" },
    { "erps ring <name> manual-switch port <0|1>", "operator manual switch" },
    { "erps ring <name> clear", "clear FS/MS, or revert at the RPL owner" },
    { "erps ring <name> flush-event", "send an R-APS flush event burst" },
    { "show erps [summary] [json]", "one line per ring" },
    { "show erps ring <name> [detail|status|config|counters|history] [json]", "ring details" },
    { "show running-config erps", "configuration as commands" },
    { "clear erps [ring <name>] counters", "reset counters" },
    { "debug erps [ring <name>] level <error|warning|notice|info|debug|trace>", "log level" },
    { "help", "this list" },
};

const erps_cli_cmd_t *erps_cli_commands(size_t *count)
{
    if (count)
        *count = sizeof commands / sizeof commands[0];
    return commands;
}

/* ------------------------------------------------------------------------------------------ */
/* Output                                                                                     */
/* ------------------------------------------------------------------------------------------ */

__attribute__((format(printf, 2, 3))) static void outf(struct out *o, const char *fmt, ...)
{
    if (!o->fn)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof buf) {
        o->fn(o->ctx, buf);
        return;
    }
    char *big = malloc((size_t)n + 1);
    if (!big)
        return;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    o->fn(o->ctx, big);
    free(big);
}

__attribute__((format(printf, 3, 4))) static erps_err_t fail(struct out *o, erps_err_t rc,
                                                            const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    outf(o, "%% %s\n", buf);
    return rc;
}

static void out_dump(struct out *o, const erps_t *e, unsigned what, erps_fmt_t fmt)
{
    int need = erps_dump(e, what, fmt, NULL, 0);
    if (need <= 0)
        return;
    char *buf = malloc((size_t)need + 1);
    if (!buf)
        return;
    erps_dump(e, what, fmt, buf, (size_t)need + 1);
    o->fn(o->ctx, buf);
    if (fmt == ERPS_FMT_JSON)
        o->fn(o->ctx, "\n");
    free(buf);
}

/* ------------------------------------------------------------------------------------------ */
/* Parsing helpers                                                                            */
/* ------------------------------------------------------------------------------------------ */

static int tokenize(char *line, char **tok, int max)
{
    int n = 0;
    char *p = line;
    while (*p) {
        while (*p && isspace((unsigned char)*p))
            p++;
        if (!*p)
            break;
        if (n == max)
            return -1;
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p))
            p++;
        if (*p)
            *p++ = '\0';
    }
    return n;
}

static bool parse_uint(const char *s, unsigned long min, unsigned long max, unsigned long *out)
{
    if (!s || !*s || !isdigit((unsigned char)*s))
        return false;
    char *end;
    errno = 0;
    unsigned long v = strtoul(s, &end, 10);
    if (errno || *end || v < min || v > max)
        return false;
    *out = v;
    return true;
}

/* Duration in milliseconds: "5m", "300s", "500ms", "1h", bare = ms. */
static bool parse_dur_ms(const char *s, uint32_t *out)
{
    if (!s || !isdigit((unsigned char)*s))
        return false;
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno)
        return false;
    unsigned long long mul;
    if (!*end || !strcmp(end, "ms"))
        mul = 1;
    else if (!strcmp(end, "s"))
        mul = 1000;
    else if (!strcmp(end, "m") || !strcmp(end, "min"))
        mul = 60000;
    else if (!strcmp(end, "h"))
        mul = 3600000;
    else
        return false;
    if (v > 0xFFFFFFFFull / mul)
        return false;
    *out = (uint32_t)(v * mul);
    return true;
}

static void fmt_dur_ms(uint32_t ms, char *b, size_t n)
{
    if (ms && ms % 3600000u == 0)
        snprintf(b, n, "%uh", ms / 3600000u);
    else if (ms && ms % 60000u == 0)
        snprintf(b, n, "%um", ms / 60000u);
    else if (ms && ms % 1000u == 0)
        snprintf(b, n, "%us", ms / 1000u);
    else
        snprintf(b, n, "%ums", ms);
}

static bool parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    char tail;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6 &&
        sscanf(s, "%x-%x-%x-%x-%x-%x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6)
        return false;
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xFF)
            return false;
        mac[i] = (uint8_t)v[i];
    }
    return true;
}

static bool parse_vlans(const char *s, erps_cli_ring_extra_t *x)
{
    erps_cli_ring_extra_t tmp;
    memset(&tmp, 0, sizeof tmp);
    if (!strcmp(s, "none")) {
        *x = tmp;
        return true;
    }
    const char *p = s;
    while (*p) {
        char *end;
        unsigned long a = strtoul(p, &end, 10), b;
        if (end == p || a < 1 || a > 4094)
            return false;
        p = end;
        b = a;
        if (*p == '-') {
            p++;
            b = strtoul(p, &end, 10);
            if (end == p || b < a || b > 4094)
                return false;
            p = end;
        }
        for (unsigned long v = a; v <= b; v++)
            tmp.vlans[v / 8] = (uint8_t)(tmp.vlans[v / 8] | (1u << (v % 8)));
        if (*p == ',')
            p++;
        else if (*p)
            return false;
    }
    *x = tmp;
    return true;
}

/* Format the VLAN set as ranges, split into chunks short enough for one command line each
 * (a fragmented set can need ~10 KB). Calls emit(chunk, first) per chunk; returns the number of
 * chunks (0 = empty set). */
static int fmt_vlans(const erps_cli_ring_extra_t *x, void (*emit)(void *, const char *, bool),
                     void *ctx)
{
    char b[1024];
    size_t pos = 0;
    int chunks = 0;
    b[0] = '\0';
    for (unsigned v = 1; v <= 4094; v++) {
        if (!erps_cli_vlan_test(x, v))
            continue;
        unsigned w = v;
        while (w + 1 <= 4094 && erps_cli_vlan_test(x, w + 1))
            w++;
        char item[16];
        if (w == v)
            snprintf(item, sizeof item, "%u", v);
        else
            snprintf(item, sizeof item, "%u-%u", v, w);
        if (pos + strlen(item) + 2 > sizeof b - 1) {
            emit(ctx, b, chunks == 0);
            chunks++;
            pos = 0;
            b[0] = '\0';
        }
        pos += (size_t)snprintf(b + pos, sizeof b - pos, "%s%s", pos ? "," : "", item);
        v = w;
    }
    if (pos) {
        emit(ctx, b, chunks == 0);
        chunks++;
    }
    return chunks;
}

static bool parse_onoff(const char *s, bool *out)
{
    if (!s)
        return false;
    if (!strcmp(s, "on") || !strcmp(s, "enable") || !strcmp(s, "true"))
        *out = true;
    else if (!strcmp(s, "off") || !strcmp(s, "disable") || !strcmp(s, "false"))
        *out = false;
    else
        return false;
    return true;
}

static bool parse_port(const char *s, erps_port_t *p)
{
    if (!s)
        return false;
    if (!strcmp(s, "0") || !strcmp(s, "port0"))
        *p = ERPS_PORT0;
    else if (!strcmp(s, "1") || !strcmp(s, "port1"))
        *p = ERPS_PORT1;
    else
        return false;
    return true;
}

static bool parse_level(const char *s, erps_log_level_t *l)
{
    for (int i = ERPS_LOG_ERR; i <= ERPS_LOG_TRACE; i++) {
        if (!strcmp(s, erps_log_level_str((erps_log_level_t)i))) {
            *l = (erps_log_level_t)i;
            return true;
        }
    }
    if (!strcmp(s, "err")) {
        *l = ERPS_LOG_ERR;
        return true;
    }
    if (!strcmp(s, "warn")) {
        *l = ERPS_LOG_WARN;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------------------------------ */
/* Rings                                                                                      */
/* ------------------------------------------------------------------------------------------ */

static struct ring *find_ring(const erps_cli_t *cli, const char *name)
{
    for (size_t i = 0; i < cli->n; i++)
        if (!strcmp(cli->rings[i].cfg.name, name))
            return &cli->rings[i];
    return NULL;
}

static struct ring *new_ring(erps_cli_t *cli, const char *name)
{
    if (cli->n == cli->cap) {
        size_t cap = cli->cap ? cli->cap * 2 : 4;
        struct ring *r = realloc(cli->rings, cap * sizeof *r);
        if (!r)
            return NULL;
        cli->rings = r;
        cli->cap = cap;
    }
    struct ring *r = &cli->rings[cli->n++];
    memset(r, 0, sizeof *r);
    erps_config_init(&r->cfg);
    snprintf(r->cfg.name, sizeof r->cfg.name, "%s", name);
    r->cfg.port[0].handle = UINT32_MAX - 1;
    r->cfg.port[1].handle = UINT32_MAX;
    return r;
}

static void delete_ring(erps_cli_t *cli, struct ring *r)
{
    size_t i = (size_t)(r - cli->rings);
    memmove(&cli->rings[i], &cli->rings[i + 1], (cli->n - i - 1) * sizeof *r);
    cli->n--;
}

static erps_time_t now_of(const erps_cli_t *cli)
{
    return cli->plat.now ? cli->plat.now(cli->plat.ctx) : 0;
}

static erps_err_t set_port(erps_cli_t *cli, struct ring *r, int idx, const char *name,
                           struct out *o)
{
    uint32_t handle;
    uint8_t mac[6] = { 0 };
    if (strlen(name) >= sizeof r->cfg.port[idx].name)
        return fail(o, ERPS_E_INVAL, "port name too long: %s", name);
    if (cli->plat.port_lookup) {
        if (cli->plat.port_lookup(cli->plat.ctx, name, &handle, mac) != 0)
            return fail(o, ERPS_E_NOTFOUND, "unknown port %s", name);
    } else {
        unsigned long v;
        if (!parse_uint(name, 0, UINT32_MAX, &v))
            return fail(o, ERPS_E_INVAL, "port must be a numeric handle: %s", name);
        handle = (uint32_t)v;
    }
    r->cfg.port[idx].handle = handle;
    memcpy(r->cfg.port[idx].mac, mac, 6);
    snprintf(r->cfg.port[idx].name, sizeof r->cfg.port[idx].name, "%s", name);
    return ERPS_OK;
}

static erps_err_t ring_enable(erps_cli_t *cli, struct ring *r, struct out *o)
{
    if (r->enabled)
        return ERPS_OK;
    if (!r->node_id_explicit) {
        if (!cli->plat.system_mac || cli->plat.system_mac(cli->plat.ctx, r->cfg.node_id) != 0)
            return fail(o, ERPS_E_CONFIG, "ring %s: node-id not set and no system MAC available",
                        r->cfg.name);
    }
    char why[200];
    if (erps_config_validate(&r->cfg, why, sizeof why) != ERPS_OK)
        return fail(o, ERPS_E_CONFIG, "ring %s: %s", r->cfg.name, why);
    if (!r->cfg.port[0].name[0] || !r->cfg.port[1].name[0])
        return fail(o, ERPS_E_CONFIG, "ring %s: port0 and port1 must be set", r->cfg.name);
    if (!cli->plat.instance_bind)
        return fail(o, ERPS_E_UNSUPPORTED, "this platform cannot create rings");
    erps_ops_t ops;
    memset(&ops, 0, sizeof ops);
    void *user = NULL;
    why[0] = '\0';
    erps_config_t run = r->cfg; /* the platform may adjust the running copy */
    if (cli->plat.instance_bind(cli->plat.ctx, &run, &r->x, &ops, &user, why, sizeof why) != 0)
        return fail(o, ERPS_E_PLATFORM, "ring %s: %s", r->cfg.name, why[0] ? why : "bind failed");
    if (erps_config_validate(&run, why, sizeof why) != ERPS_OK) {
        if (cli->plat.instance_unbind)
            cli->plat.instance_unbind(cli->plat.ctx, user);
        return fail(o, ERPS_E_CONFIG, "ring %s: platform config: %s", r->cfg.name, why);
    }
    erps_err_t rc = erps_mgr_add(cli->mgr, &run, &ops, user, &r->e);
    if (rc != ERPS_OK) {
        if (cli->plat.instance_unbind)
            cli->plat.instance_unbind(cli->plat.ctx, user);
        return fail(o, rc, "ring %s: %s", r->cfg.name,
                    rc == ERPS_E_EXISTS ? "(ring-id, raps-vid) already used on these ports"
                                        : erps_strerror(rc));
    }
    r->user = user;
    rc = erps_start(r->e, now_of(cli));
    if (rc != ERPS_OK) {
        erps_mgr_remove(cli->mgr, r->e);
        r->e = NULL;
        if (cli->plat.instance_unbind)
            cli->plat.instance_unbind(cli->plat.ctx, user);
        return fail(o, rc, "ring %s: start failed: %s", r->cfg.name, erps_strerror(rc));
    }
    r->enabled = true;
    return ERPS_OK;
}

static void ring_disable(erps_cli_t *cli, struct ring *r)
{
    if (!r->enabled || r->external)
        return;
    erps_stop(r->e, now_of(cli), true);
    erps_mgr_remove(cli->mgr, r->e);
    if (cli->plat.instance_unbind)
        cli->plat.instance_unbind(cli->plat.ctx, r->user);
    r->e = NULL;
    r->user = NULL;
    r->enabled = false;
}

/* Configuration keywords; when the ring is enabled only live-changeable ones are accepted. */
static erps_err_t ring_config(erps_cli_t *cli, struct ring *r, char **t, int n, struct out *o)
{
    erps_config_t c = r->cfg;
    erps_cli_ring_extra_t x = r->x;
    bool node_explicit = r->node_id_explicit;
    bool timers_changed = false, revert_changed = false, restart_needed = false;

    for (int i = 0; i < n; i++) {
        const char *k = t[i];
        const char *v = i + 1 < n ? t[i + 1] : NULL;
        unsigned long u;
        uint32_t ms;
        bool b;
#define NEED_VALUE()                                                                         \
    do {                                                                                     \
        if (!v)                                                                              \
            return fail(o, ERPS_E_INVAL, "%s: missing value", k);                            \
        i++;                                                                                 \
    } while (0)
        if (!strcmp(k, "timers")) {
            continue;
        } else if (!strcmp(k, "ring-id")) {
            NEED_VALUE();
            if (!parse_uint(v, 1, 239, &u))
                return fail(o, ERPS_E_INVAL, "ring-id must be 1..239");
            c.ring_id = (uint8_t)u;
            restart_needed = true;
        } else if (!strcmp(k, "raps-vid")) {
            NEED_VALUE();
            if (!parse_uint(v, 1, 4094, &u))
                return fail(o, ERPS_E_INVAL, "raps-vid must be 1..4094");
            c.raps_vid = (uint16_t)u;
            restart_needed = true;
        } else if (!strcmp(k, "mel")) {
            NEED_VALUE();
            if (!parse_uint(v, 0, 7, &u))
                return fail(o, ERPS_E_INVAL, "mel must be 0..7");
            c.mel = (uint8_t)u;
            restart_needed = true;
        } else if (!strcmp(k, "pcp")) {
            NEED_VALUE();
            if (!parse_uint(v, 0, 7, &u))
                return fail(o, ERPS_E_INVAL, "pcp must be 0..7");
            c.raps_pcp = (uint8_t)u;
            restart_needed = true;
        } else if (!strcmp(k, "port0") || !strcmp(k, "port1")) {
            NEED_VALUE();
            if (r->enabled)
                return fail(o, ERPS_E_STATE, "ring %s is enabled; disable it to change ports",
                            c.name);
            struct ring tmp = *r;
            tmp.cfg = c;
            erps_err_t rc = set_port(cli, &tmp, k[4] - '0', v, o);
            if (rc != ERPS_OK)
                return rc;
            c = tmp.cfg;
        } else if (!strcmp(k, "node-id")) {
            NEED_VALUE();
            if (!strcmp(v, "auto")) {
                node_explicit = false;
            } else {
                if (!parse_mac(v, c.node_id))
                    return fail(o, ERPS_E_INVAL, "node-id must be a MAC address or 'auto'");
                node_explicit = true;
            }
            restart_needed = true;
        } else if (!strcmp(k, "role")) {
            NEED_VALUE();
            if (!strcmp(v, "owner") || !strcmp(v, "rpl-owner"))
                c.role = ERPS_ROLE_RPL_OWNER;
            else if (!strcmp(v, "neighbour") || !strcmp(v, "neighbor") ||
                     !strcmp(v, "rpl-neighbour"))
                c.role = ERPS_ROLE_RPL_NEIGHBOUR;
            else if (!strcmp(v, "none"))
                c.role = ERPS_ROLE_NONE;
            else
                return fail(o, ERPS_E_INVAL, "role must be owner, neighbour or none");
            restart_needed = true;
        } else if (!strcmp(k, "rpl-port")) {
            NEED_VALUE();
            if (!parse_port(v, &c.rpl_port))
                return fail(o, ERPS_E_INVAL, "rpl-port must be 0 or 1");
            restart_needed = true;
        } else if (!strcmp(k, "revertive") || !strcmp(k, "non-revertive")) {
            c.revertive = k[0] == 'r';
            revert_changed = true;
        } else if (!strcmp(k, "compat-version")) {
            NEED_VALUE();
            if (!parse_uint(v, 1, 2, &u))
                return fail(o, ERPS_E_INVAL, "compat-version must be 1 or 2");
            c.compat_version = (uint8_t)u;
            restart_needed = true;
        } else if (!strcmp(k, "wtr") || !strcmp(k, "guard") || !strcmp(k, "holdoff") ||
                   !strcmp(k, "hold-off")) {
            NEED_VALUE();
            if (!parse_dur_ms(v, &ms))
                return fail(o, ERPS_E_INVAL, "%s: bad duration '%s' (e.g. 5m, 500ms, 2s)", k, v);
            if (k[0] == 'w')
                c.wtr_ms = ms;
            else if (k[0] == 'g')
                c.guard_ms = ms;
            else
                c.holdoff_ms = ms;
            timers_changed = true;
        } else if (!strcmp(k, "fop-timeout")) {
            NEED_VALUE();
            if (!parse_dur_ms(v, &ms))
                return fail(o, ERPS_E_INVAL, "fop-timeout: bad duration '%s'", v);
            c.fop_timeout_ms = ms;
            restart_needed = true;
        } else if (!strcmp(k, "protected-vlans")) {
            NEED_VALUE();
            /* protected-vlans <list> replaces; protected-vlans add|remove <list> edits */
            if (!strcmp(v, "add") || !strcmp(v, "remove")) {
                const char *op = v;
                v = i + 1 < n ? t[i + 1] : NULL;
                NEED_VALUE();
                erps_cli_ring_extra_t delta;
                if (!parse_vlans(v, &delta))
                    return fail(o, ERPS_E_INVAL, "protected-vlans %s: bad list '%s'", op, v);
                for (size_t j = 0; j < sizeof x.vlans; j++)
                    x.vlans[j] = op[0] == 'a' ? (uint8_t)(x.vlans[j] | delta.vlans[j])
                                              : (uint8_t)(x.vlans[j] & ~delta.vlans[j]);
            } else if (!parse_vlans(v, &x)) {
                return fail(o, ERPS_E_INVAL, "protected-vlans: bad list '%s' (e.g. 10-20,30)", v);
            }
            restart_needed = true;
        } else if (!strcmp(k, "init-timer")) {
            NEED_VALUE();
            if (!strcmp(v, "wtb"))
                c.init_wtb = true;
            else if (!strcmp(v, "wtr"))
                c.init_wtb = false;
            else
                return fail(o, ERPS_E_INVAL, "init-timer must be wtr or wtb");
            restart_needed = true;
        } else if (!strcmp(k, "sw-forwarding") || !strcmp(k, "fop-pm") || !strcmp(k, "fop-to") ||
                   !strcmp(k, "relaxed-timers")) {
            NEED_VALUE();
            if (!parse_onoff(v, &b))
                return fail(o, ERPS_E_INVAL, "%s: expected on|off", k);
            if (!strcmp(k, "sw-forwarding"))
                c.sw_raps_forwarding = b;
            else if (!strcmp(k, "fop-pm"))
                c.fop_pm_enable = b;
            else if (!strcmp(k, "fop-to"))
                c.fop_to_enable = b;
            else
                c.relaxed_timers = b;
            restart_needed = true;
        } else {
            return fail(o, ERPS_E_INVAL, "unknown keyword '%s' (try 'help')", k);
        }
#undef NEED_VALUE
    }

    if (r->enabled && restart_needed)
        return fail(o, ERPS_E_STATE,
                    "ring %s is enabled: only timers and revertive mode change live; "
                    "disable it first",
                    c.name);
    if (timers_changed) {
        char why[200];
        if (erps_config_validate_timers(&c, why, sizeof why) != ERPS_OK)
            return fail(o, ERPS_E_CONFIG, "%s", why);
    }
    if (r->enabled) {
        if (timers_changed) {
            erps_err_t rc = erps_set_timers(r->e, c.wtr_ms, c.guard_ms, c.holdoff_ms);
            if (rc != ERPS_OK)
                return fail(o, rc, "timers rejected");
        }
        if (revert_changed)
            erps_set_revertive(r->e, c.revertive);
    }
    r->cfg = c;
    r->x = x;
    r->node_id_explicit = node_explicit;
    return ERPS_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* show                                                                                       */
/* ------------------------------------------------------------------------------------------ */

static void show_summary(erps_cli_t *cli, struct out *o, bool json)
{
    if (json) {
        outf(o, "{\"rings\":[");
        for (size_t i = 0; i < cli->n; i++) {
            const struct ring *r = &cli->rings[i];
            erps_status_t st;
            memset(&st, 0, sizeof st);
            if (r->e)
                erps_get_status(r->e, &st);
            char top[112];
            erps_req_format(&st.top, top, sizeof top);
            outf(o,
                 "%s{\"name\":\"%s\",\"enabled\":%s,\"ring_id\":%u,\"raps_vid\":%u,"
                 "\"role\":\"%s\",\"state\":\"%s\",\"ports\":[{\"name\":\"%s\",\"blocked\":%s},"
                 "{\"name\":\"%s\",\"blocked\":%s}],\"top_request\":\"%s\"}",
                 i ? "," : "", r->cfg.name, r->enabled ? "true" : "false", r->cfg.ring_id,
                 r->cfg.raps_vid, erps_role_str(r->cfg.role),
                 r->e ? erps_state_str(st.state) : "Disabled", r->cfg.port[0].name,
                 st.port[0].blocked ? "true" : "false", r->cfg.port[1].name,
                 st.port[1].blocked ? "true" : "false", top);
        }
        outf(o, "]}\n");
        return;
    }
    outf(o, "%-12s %-4s %-5s %-13s %-14s %-18s %-18s %s\n", "Ring", "ID", "VID", "Role", "State",
         "Port0", "Port1", "Top request");
    for (size_t i = 0; i < cli->n; i++) {
        const struct ring *r = &cli->rings[i];
        erps_status_t st;
        memset(&st, 0, sizeof st);
        char p0[40], p1[40], top[112] = "-";
        if (r->e) {
            erps_get_status(r->e, &st);
            erps_req_format(&st.top, top, sizeof top);
        }
        snprintf(p0, sizeof p0, "%s:%s", r->cfg.port[0].name[0] ? r->cfg.port[0].name : "-",
                 !r->e ? "-" : st.port[0].blocked ? "B" : "F");
        snprintf(p1, sizeof p1, "%s:%s", r->cfg.port[1].name[0] ? r->cfg.port[1].name : "-",
                 !r->e ? "-" : st.port[1].blocked ? "B" : "F");
        outf(o, "%-12s %-4u %-5u %-13s %-14s %-18s %-18s %s\n", r->cfg.name, r->cfg.ring_id,
             r->cfg.raps_vid, erps_role_str(r->cfg.role), r->e ? erps_state_str(st.state) : "Disabled",
             p0, p1, r->e ? top : "-");
    }
    if (!cli->n)
        outf(o, "(no rings configured)\n");
}

struct vlan_emit {
    struct out *o;
    const char *ring;
};

static void emit_vlan_line(void *ctx, const char *chunk, bool first)
{
    struct vlan_emit *ve = ctx;
    outf(ve->o, "erps ring %s protected-vlans %s%s\n", ve->ring, first ? "" : "add ", chunk);
}

static void running_config(erps_cli_t *cli, struct out *o)
{
    for (size_t i = 0; i < cli->n; i++) {
        const struct ring *r = &cli->rings[i];
        if (r->external)
            continue;
        const erps_config_t *c = &r->cfg;
        const char *nm = c->name;
        outf(o, "erps ring %s ring-id %u raps-vid %u mel %u", nm, c->ring_id, c->raps_vid, c->mel);
        if (c->port[0].name[0])
            outf(o, " port0 %s", c->port[0].name);
        if (c->port[1].name[0])
            outf(o, " port1 %s", c->port[1].name);
        outf(o, "\n");
        if (r->node_id_explicit)
            outf(o, "erps ring %s node-id %02x:%02x:%02x:%02x:%02x:%02x\n", nm, c->node_id[0],
                 c->node_id[1], c->node_id[2], c->node_id[3], c->node_id[4], c->node_id[5]);
        if (c->role != ERPS_ROLE_NONE)
            outf(o, "erps ring %s role %s rpl-port %d\n", nm,
                 c->role == ERPS_ROLE_RPL_OWNER ? "owner" : "neighbour", (int)c->rpl_port);
        char w[16], g[16], h[16];
        fmt_dur_ms(c->wtr_ms, w, sizeof w);
        fmt_dur_ms(c->guard_ms, g, sizeof g);
        fmt_dur_ms(c->holdoff_ms, h, sizeof h);
        if (c->relaxed_timers)
            outf(o, "erps ring %s relaxed-timers on\n", nm);
        outf(o, "erps ring %s timers wtr %s guard %s holdoff %s\n", nm, w, g, h);
        if (!c->revertive)
            outf(o, "erps ring %s non-revertive\n", nm);
        if (c->init_wtb)
            outf(o, "erps ring %s init-timer wtb\n", nm);
        if (c->compat_version != 2)
            outf(o, "erps ring %s compat-version %u\n", nm, c->compat_version);
        if (c->raps_pcp != 7)
            outf(o, "erps ring %s pcp %u\n", nm, c->raps_pcp);
        struct vlan_emit ve = { o, nm };
        fmt_vlans(&r->x, emit_vlan_line, &ve);
        if (c->sw_raps_forwarding)
            outf(o, "erps ring %s sw-forwarding on\n", nm);
        if (!c->fop_pm_enable)
            outf(o, "erps ring %s fop-pm off\n", nm);
        if (c->fop_to_enable)
            outf(o, "erps ring %s fop-to on\n", nm);
        if (c->fop_timeout_ms != 17500) {
            char ft[16];
            fmt_dur_ms(c->fop_timeout_ms, ft, sizeof ft);
            outf(o, "erps ring %s fop-timeout %s\n", nm, ft);
        }
        if (r->enabled)
            outf(o, "erps ring %s enable\n", nm);
        outf(o, "!\n");
    }
}

static erps_err_t cmd_show(erps_cli_t *cli, char **t, int n, struct out *o)
{
    /* t[0] = "show" */
    if (n >= 2 && !strcmp(t[1], "running-config")) {
        if (n == 2 || (n == 3 && !strcmp(t[2], "erps"))) {
            running_config(cli, o);
            return ERPS_OK;
        }
        return fail(o, ERPS_E_INVAL, "usage: show running-config erps");
    }
    if (n < 2 || strcmp(t[1], "erps"))
        return fail(o, ERPS_E_INVAL, "usage: show erps ...");
    bool json = n > 2 && !strcmp(t[n - 1], "json");
    if (json)
        n--;
    if (n == 2 || (n == 3 && !strcmp(t[2], "summary"))) {
        show_summary(cli, o, json);
        return ERPS_OK;
    }
    if (n == 3 && !strcmp(t[2], "running-config")) {
        running_config(cli, o);
        return ERPS_OK;
    }
    if (n >= 4 && !strcmp(t[2], "ring")) {
        struct ring *r = find_ring(cli, t[3]);
        if (!r)
            return fail(o, ERPS_E_NOTFOUND, "no ring %s", t[3]);
        unsigned what = ERPS_DUMP_CONFIG | ERPS_DUMP_STATUS;
        if (n >= 5) {
            if (!strcmp(t[4], "detail"))
                what = ERPS_DUMP_ALL;
            else if (!strcmp(t[4], "status"))
                what = ERPS_DUMP_STATUS;
            else if (!strcmp(t[4], "config"))
                what = ERPS_DUMP_CONFIG;
            else if (!strcmp(t[4], "counters"))
                what = ERPS_DUMP_COUNTERS;
            else if (!strcmp(t[4], "history"))
                what = ERPS_DUMP_HISTORY;
            else
                return fail(o, ERPS_E_INVAL, "unknown view '%s'", t[4]);
        }
        if (!r->e) {
            if (json) {
                outf(o, "{\"name\":\"%s\",\"enabled\":false}\n", r->cfg.name);
            } else {
                outf(o, "ERP instance %s: disabled\n", r->cfg.name);
                running_config(cli, o);
            }
            return ERPS_OK;
        }
        out_dump(o, r->e, what, json ? ERPS_FMT_JSON : ERPS_FMT_TEXT);
        return ERPS_OK;
    }
    return fail(o, ERPS_E_INVAL, "usage: show erps [summary] | show erps ring <name> [view] [json]");
}

/* ------------------------------------------------------------------------------------------ */
/* Dispatch                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static erps_err_t cmd_ring(erps_cli_t *cli, char **t, int n, struct out *o)
{
    /* t[0]="erps" t[1]="ring" t[2]=name ... */
    if (n < 4)
        return fail(o, ERPS_E_INVAL, "usage: erps ring <name> <keyword> ...");
    const char *name = t[2];
    if (strlen(name) >= ERPS_NAME_MAX)
        return fail(o, ERPS_E_INVAL, "ring name too long (max %d)", ERPS_NAME_MAX - 1);
    struct ring *r = find_ring(cli, name);
    const char *k = t[3];

    if (!strcmp(k, "enable") || !strcmp(k, "disable")) {
        if (!r)
            return fail(o, ERPS_E_NOTFOUND, "no ring %s", name);
        if (r->external)
            return fail(o, ERPS_E_UNSUPPORTED, "ring %s is managed externally", name);
        if (k[0] == 'e')
            return ring_enable(cli, r, o);
        ring_disable(cli, r);
        return ERPS_OK;
    }

    if (!strcmp(k, "force-switch") || !strcmp(k, "manual-switch") || !strcmp(k, "clear") ||
        !strcmp(k, "flush-event")) {
        if (!r)
            return fail(o, ERPS_E_NOTFOUND, "no ring %s", name);
        if (!r->e)
            return fail(o, ERPS_E_STATE, "ring %s is not enabled", name);
        if (!strcmp(k, "flush-event")) {
            erps_err_t rc = erps_send_flush_event(r->e, now_of(cli));
            return rc == ERPS_OK ? rc : fail(o, rc, "%s", erps_strerror(rc));
        }
        erps_cmd_t cmd = k[0] == 'f' ? ERPS_CMD_FORCED_SWITCH
                         : k[0] == 'm' ? ERPS_CMD_MANUAL_SWITCH
                                       : ERPS_CMD_CLEAR;
        erps_port_t port = ERPS_PORT_NONE;
        if (cmd != ERPS_CMD_CLEAR) {
            const char *ps = n >= 6 && !strcmp(t[4], "port") ? t[5] : (n >= 5 ? t[4] : NULL);
            if (!parse_port(ps, &port))
                return fail(o, ERPS_E_INVAL, "usage: erps ring %s %s port <0|1>", name, k);
        }
        erps_reject_t why = ERPS_REJECT_NONE;
        erps_err_t rc = erps_command(r->e, cmd, port, now_of(cli), &why);
        if (rc == ERPS_E_REJECTED)
            return fail(o, rc, "%s rejected: %s", erps_cmd_str(cmd), erps_reject_str(why));
        if (rc != ERPS_OK)
            return fail(o, rc, "%s", erps_strerror(rc));
        return ERPS_OK;
    }

    if (r && r->external)
        return fail(o, ERPS_E_UNSUPPORTED, "ring %s is managed externally", name);
    bool created = false;
    if (!r) {
        r = new_ring(cli, name);
        if (!r)
            return fail(o, ERPS_E_NOMEM, "out of memory");
        created = true;
    }
    erps_err_t rc = ring_config(cli, r, t + 3, n - 3, o);
    if (rc != ERPS_OK && created)
        delete_ring(cli, r);
    return rc;
}

static erps_err_t cmd_clear(erps_cli_t *cli, char **t, int n, struct out *o)
{
    /* clear erps [ring <name>] counters */
    if (n == 3 && !strcmp(t[2], "counters")) {
        for (size_t i = 0; i < cli->n; i++)
            if (cli->rings[i].e)
                erps_clear_counters(cli->rings[i].e);
        return ERPS_OK;
    }
    if (n == 5 && !strcmp(t[2], "ring") && !strcmp(t[4], "counters")) {
        struct ring *r = find_ring(cli, t[3]);
        if (!r)
            return fail(o, ERPS_E_NOTFOUND, "no ring %s", t[3]);
        if (r->e)
            erps_clear_counters(r->e);
        return ERPS_OK;
    }
    return fail(o, ERPS_E_INVAL, "usage: clear erps [ring <name>] counters");
}

static erps_err_t cmd_debug(erps_cli_t *cli, char **t, int n, struct out *o)
{
    /* debug erps [ring <name>] level <lvl> */
    erps_log_level_t lvl;
    if (n == 4 && !strcmp(t[2], "level") && parse_level(t[3], &lvl)) {
        for (size_t i = 0; i < cli->n; i++) {
            cli->rings[i].cfg.log_level = lvl;
            if (cli->rings[i].e)
                erps_set_log_level(cli->rings[i].e, lvl);
        }
        return ERPS_OK;
    }
    if (n == 6 && !strcmp(t[2], "ring") && !strcmp(t[4], "level") && parse_level(t[5], &lvl)) {
        struct ring *r = find_ring(cli, t[3]);
        if (!r)
            return fail(o, ERPS_E_NOTFOUND, "no ring %s", t[3]);
        r->cfg.log_level = lvl;
        if (r->e)
            erps_set_log_level(r->e, lvl);
        return ERPS_OK;
    }
    return fail(o, ERPS_E_INVAL,
                "usage: debug erps [ring <name>] level <error|warning|notice|info|debug|trace>");
}

erps_err_t erps_cli_exec(erps_cli_t *cli, const char *line, erps_cli_out_fn out, void *out_ctx)
{
    struct out o = { out, out_ctx };
    if (!cli || !line)
        return ERPS_E_INVAL;
    if (strlen(line) >= MAX_LINE)
        return fail(&o, ERPS_E_INVAL, "line too long");
    char buf[MAX_LINE];
    snprintf(buf, sizeof buf, "%s", line);
    char *tok[MAX_TOKENS];
    int n = tokenize(buf, tok, MAX_TOKENS);
    if (n < 0)
        return fail(&o, ERPS_E_INVAL, "too many words");
    if (n == 0 || tok[0][0] == '#' || tok[0][0] == '!')
        return ERPS_OK;

    if (!strcmp(tok[0], "help") || !strcmp(tok[0], "?")) {
        size_t k;
        const erps_cli_cmd_t *c = erps_cli_commands(&k);
        for (size_t i = 0; i < k; i++)
            outf(&o, "  %-72s %s\n", c[i].syntax, c[i].help);
        return ERPS_OK;
    }
    if (!strcmp(tok[0], "show"))
        return cmd_show(cli, tok, n, &o);
    if (!strcmp(tok[0], "clear") && n >= 2 && !strcmp(tok[1], "erps"))
        return cmd_clear(cli, tok, n, &o);
    if (!strcmp(tok[0], "debug") && n >= 2 && !strcmp(tok[1], "erps"))
        return cmd_debug(cli, tok, n, &o);
    if (!strcmp(tok[0], "no") && n == 4 && !strcmp(tok[1], "erps") && !strcmp(tok[2], "ring")) {
        struct ring *r = find_ring(cli, tok[3]);
        if (!r)
            return fail(&o, ERPS_E_NOTFOUND, "no ring %s", tok[3]);
        if (r->external)
            return fail(&o, ERPS_E_UNSUPPORTED, "ring %s is managed externally", tok[3]);
        ring_disable(cli, r);
        delete_ring(cli, r);
        return ERPS_OK;
    }
    if (!strcmp(tok[0], "erps") && n >= 2 && !strcmp(tok[1], "ring"))
        return cmd_ring(cli, tok, n, &o);
    return fail(&o, ERPS_E_INVAL, "unknown command '%s' (try 'help')", tok[0]);
}

erps_err_t erps_cli_exec_script(erps_cli_t *cli, const char *script, erps_cli_out_fn out,
                                void *out_ctx, int *err_line)
{
    if (!cli || !script)
        return ERPS_E_INVAL;
    int lineno = 0;
    const char *p = script;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[MAX_LINE];
        lineno++;
        if (len >= sizeof line) {
            if (err_line)
                *err_line = lineno;
            return ERPS_E_INVAL;
        }
        memcpy(line, p, len);
        line[len] = '\0';
        if (len && line[len - 1] == '\r')
            line[len - 1] = '\0';
        erps_err_t rc = erps_cli_exec(cli, line, out, out_ctx);
        if (rc != ERPS_OK) {
            if (err_line)
                *err_line = lineno;
            return rc;
        }
        p = nl ? nl + 1 : p + len;
    }
    return ERPS_OK;
}

/* ------------------------------------------------------------------------------------------ */
/* Lifecycle                                                                                  */
/* ------------------------------------------------------------------------------------------ */

erps_cli_t *erps_cli_create(const erps_cli_platform_t *platform)
{
    erps_cli_t *cli = calloc(1, sizeof *cli);
    if (!cli)
        return NULL;
    if (platform)
        cli->plat = *platform;
    cli->mgr = erps_mgr_create();
    if (!cli->mgr) {
        free(cli);
        return NULL;
    }
    return cli;
}

void erps_cli_destroy(erps_cli_t *cli)
{
    if (!cli)
        return;
    for (size_t i = 0; i < cli->n; i++)
        ring_disable(cli, &cli->rings[i]);
    erps_mgr_destroy(cli->mgr);
    free(cli->rings);
    free(cli);
}

erps_err_t erps_cli_attach(erps_cli_t *cli, erps_t *e)
{
    if (!cli || !e)
        return ERPS_E_INVAL;
    const erps_config_t *c = erps_get_config(e);
    if (!c)
        return ERPS_E_INVAL;
    if (find_ring(cli, c->name))
        return ERPS_E_EXISTS;
    struct ring *r = new_ring(cli, c->name);
    if (!r)
        return ERPS_E_NOMEM;
    r->cfg = *c;
    r->external = true;
    r->enabled = true;
    r->node_id_explicit = true;
    r->e = e;
    return ERPS_OK;
}

erps_mgr_t *erps_cli_mgr(erps_cli_t *cli)
{
    return cli ? cli->mgr : NULL;
}

const erps_cli_ring_extra_t *erps_cli_ring_extra(const erps_cli_t *cli, const char *name)
{
    const struct ring *r = cli ? find_ring(cli, name) : NULL;
    return r ? &r->x : NULL;
}
