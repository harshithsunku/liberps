/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Discrete-event ring simulator — see include/erps/erps_sim.h.
 *
 * This file is also the reference example of a platform integration: each node implements
 * erps_ops_t (tx / set_port_state / flush_fdb / notify / log) and drives its instance with
 * erps_rx_frame(), erps_set_signal_fail() and erps_poll() at erps_next_deadline().
 */
#include "erps/erps_sim.h"
#include "erps/erps_pdu.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define SIM_MAX_FRAME 128

enum ev_type { EV_FRAME, EV_SSF };

struct sim_ev {
    erps_time_t t;
    uint64_t seq;
    enum ev_type type;
    unsigned node;
    erps_port_t port;
    uint16_t len;
    uint8_t frame[SIM_MAX_FRAME];
};

struct sim_node {
    erps_sim_t *sim;
    unsigned idx;
    erps_t *e;
    erps_config_t cfg;
    bool up;
    bool started;
    bool blocked[2];
    bool ssf_reported[2];
    uint64_t flushes;
};

struct sim_link {
    bool up[2];              /* [0] CW (i -> i+1), [1] CCW (i+1 -> i) */
    erps_time_t last_t[2];   /* FIFO per direction */
};

struct erps_sim {
    erps_sim_cfg_t cfg;
    unsigned n;
    struct sim_node *node;
    struct sim_link *link;
    erps_time_t now;
    uint64_t rng;
    uint64_t seq;

    struct sim_ev *heap;
    size_t heap_len, heap_cap;

    uint64_t delivered;
    unsigned violations;
    char first_violation[256];
    erps_time_t last_change;

    FILE *pcap;
    FILE *log;
    erps_log_level_t log_level;
    erps_sim_event_fn hook;
    void *hook_ctx;
};

/* ------------------------------------------------------------------------------------------ */
/* Utilities                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static uint64_t rng_next(erps_sim_t *s)
{
    /* xorshift64* */
    uint64_t x = s->rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s->rng = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static double rng_unit(erps_sim_t *s)
{
    return (double)(rng_next(s) >> 11) * (1.0 / 9007199254740992.0);
}

static bool ev_before(const struct sim_ev *a, const struct sim_ev *b)
{
    return a->t < b->t || (a->t == b->t && a->seq < b->seq);
}

static void heap_push(erps_sim_t *s, const struct sim_ev *ev)
{
    if (s->heap_len == s->heap_cap) {
        size_t cap = s->heap_cap ? s->heap_cap * 2 : 256;
        struct sim_ev *h = realloc(s->heap, cap * sizeof *h);
        if (!h)
            abort();
        s->heap = h;
        s->heap_cap = cap;
    }
    size_t i = s->heap_len++;
    s->heap[i] = *ev;
    s->heap[i].seq = s->seq++;
    while (i > 0) {
        size_t parent = (i - 1) / 2;
        if (!ev_before(&s->heap[i], &s->heap[parent]))
            break;
        struct sim_ev tmp = s->heap[i];
        s->heap[i] = s->heap[parent];
        s->heap[parent] = tmp;
        i = parent;
    }
}

static void heap_pop(erps_sim_t *s, struct sim_ev *out)
{
    *out = s->heap[0];
    s->heap[0] = s->heap[--s->heap_len];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < s->heap_len && ev_before(&s->heap[l], &s->heap[m]))
            m = l;
        if (r < s->heap_len && ev_before(&s->heap[r], &s->heap[m]))
            m = r;
        if (m == i)
            break;
        struct sim_ev tmp = s->heap[i];
        s->heap[i] = s->heap[m];
        s->heap[m] = tmp;
        i = m;
    }
}

static void simlog(erps_sim_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void simlog(erps_sim_t *s, const char *fmt, ...)
{
    if (!s->log)
        return;
    va_list ap;
    fprintf(s->log, "[%5llu.%06llu] ", (unsigned long long)(s->now / 1000000u),
            (unsigned long long)(s->now % 1000000u));
    va_start(ap, fmt);
    vfprintf(s->log, fmt, ap);
    va_end(ap);
    fputc('\n', s->log);
}

/* Link and direction a frame takes when sent by node n on port p. */
static void route(const erps_sim_t *s, unsigned n, erps_port_t p, unsigned *link, int *dir,
                  unsigned *dst, erps_port_t *dport)
{
    if (p == ERPS_PORT1) {
        *link = n;
        *dir = 0; /* CW */
        *dst = (n + 1) % s->n;
        *dport = ERPS_PORT0;
    } else {
        *link = (n + s->n - 1) % s->n;
        *dir = 1; /* CCW */
        *dst = (n + s->n - 1) % s->n;
        *dport = ERPS_PORT1;
    }
}

/* SSF of node n port p: the direction towards it is down or the peer node is down. */
static bool ssf_of(const erps_sim_t *s, unsigned n, erps_port_t p)
{
    unsigned link, dst;
    int dir;
    erps_port_t dport;
    route(s, n, p, &link, &dir, &dst, &dport);
    int in_dir = dir ^ 1; /* frames arriving on (n,p) travel the opposite direction */
    return !s->link[link].up[in_dir] || !s->node[dst].up;
}

static void schedule_ssf_checks(erps_sim_t *s)
{
    for (unsigned n = 0; n < s->n; n++) {
        for (int p = 0; p < 2; p++) {
            struct sim_ev ev;
            memset(&ev, 0, sizeof ev);
            ev.t = s->now + s->cfg.sf_detect_delay;
            ev.type = EV_SSF;
            ev.node = n;
            ev.port = (erps_port_t)p;
            heap_push(s, &ev);
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* pcap                                                                                        */
/* ------------------------------------------------------------------------------------------ */

static void put32(FILE *f, uint32_t v)
{
    fwrite(&v, 4, 1, f); /* native byte order; readers detect it from the magic */
}

int erps_sim_pcap_open(erps_sim_t *s, const char *path)
{
    erps_sim_pcap_close(s);
    s->pcap = fopen(path, "wb");
    if (!s->pcap)
        return -1;
    put32(s->pcap, 0xa1b2c3d4u);
    uint16_t v[2] = { 2, 4 };
    fwrite(v, 2, 2, s->pcap);
    put32(s->pcap, 0); /* thiszone */
    put32(s->pcap, 0); /* sigfigs */
    put32(s->pcap, 65535);
    put32(s->pcap, 1); /* DLT_EN10MB */
    return 0;
}

void erps_sim_pcap_close(erps_sim_t *s)
{
    if (s->pcap) {
        fclose(s->pcap);
        s->pcap = NULL;
    }
}

static void pcap_write(erps_sim_t *s, const uint8_t *frame, size_t len)
{
    if (!s->pcap)
        return;
    put32(s->pcap, (uint32_t)(s->now / 1000000u));
    put32(s->pcap, (uint32_t)(s->now % 1000000u));
    put32(s->pcap, (uint32_t)len);
    put32(s->pcap, (uint32_t)len);
    fwrite(frame, 1, len, s->pcap);
}

/* ------------------------------------------------------------------------------------------ */
/* Platform callbacks (one set per node)                                                       */
/* ------------------------------------------------------------------------------------------ */

static int node_tx(void *user, erps_port_t port, const uint8_t *frame, size_t len)
{
    struct sim_node *nd = user;
    erps_sim_t *s = nd->sim;
    if (!nd->up || len > SIM_MAX_FRAME)
        return 0;
    pcap_write(s, frame, len);
    unsigned link, dst;
    int dir;
    erps_port_t dport;
    route(s, nd->idx, port, &link, &dir, &dst, &dport);
    struct sim_link *l = &s->link[link];
    if (!l->up[dir])
        return 0; /* the wire is cut: the frame is lost */
    if (s->cfg.loss > 0 && rng_unit(s) < s->cfg.loss)
        return 0;
    erps_time_t t = s->now + s->cfg.link_delay;
    if (s->cfg.link_jitter)
        t += rng_next(s) % (s->cfg.link_jitter + 1);
    if (t < l->last_t[dir])
        t = l->last_t[dir]; /* links do not reorder */
    l->last_t[dir] = t;
    struct sim_ev ev;
    memset(&ev, 0, sizeof ev);
    ev.t = t;
    ev.type = EV_FRAME;
    ev.node = dst;
    ev.port = dport;
    ev.len = (uint16_t)len;
    memcpy(ev.frame, frame, len);
    heap_push(s, &ev);
    return 0;
}

static int node_set_port_state(void *user, erps_port_t port, unsigned mask)
{
    struct sim_node *nd = user;
    bool b = mask != 0;
    if (nd->blocked[port] != b) {
        nd->blocked[port] = b;
        nd->sim->last_change = nd->sim->now;
    }
    return 0;
}

static int node_flush(void *user)
{
    struct sim_node *nd = user;
    nd->flushes++;
    return 0;
}

static void node_notify(void *user, const erps_event_t *ev)
{
    struct sim_node *nd = user;
    if (ev->type == ERPS_EV_STATE_CHANGE)
        nd->sim->last_change = nd->sim->now;
    if (nd->sim->hook)
        nd->sim->hook(nd->sim->hook_ctx, nd->idx, ev);
}

static void node_log(void *user, erps_log_level_t lvl, const char *msg)
{
    struct sim_node *nd = user;
    (void)lvl;
    simlog(nd->sim, "n%u %s", nd->idx, msg);
}

static const erps_ops_t sim_ops = {
    .tx = node_tx,
    .set_port_state = node_set_port_state,
    .flush_fdb = node_flush,
    .notify = node_notify,
    .log = node_log,
};

/* ------------------------------------------------------------------------------------------ */
/* Lifecycle                                                                                  */
/* ------------------------------------------------------------------------------------------ */

void erps_sim_cfg_init(erps_sim_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->nodes = 4;
    c->link_delay = ERPS_USEC(50);
    c->link_jitter = 0;
    c->loss = 0;
    c->sf_detect_delay = ERPS_MSEC(1);
    c->seed = 1;
    c->sw_forwarding = true;
    c->owner = 3;
    c->neighbour = true;
    erps_config_init(&c->node_template);
}

erps_sim_t *erps_sim_create(const erps_sim_cfg_t *cfg)
{
    if (!cfg || cfg->nodes < 2 || cfg->nodes > 255 || cfg->owner >= cfg->nodes)
        return NULL;
    erps_sim_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->cfg = *cfg;
    s->n = cfg->nodes;
    s->rng = cfg->seed ? cfg->seed : 1;
    s->node = calloc(s->n, sizeof *s->node);
    s->link = calloc(s->n, sizeof *s->link);
    if (!s->node || !s->link) {
        erps_sim_destroy(s);
        return NULL;
    }
    unsigned owner = cfg->owner;
    unsigned neigh = (owner + 1) % s->n; /* the other end of the RPL (link `owner`) */
    for (unsigned i = 0; i < s->n; i++) {
        struct sim_node *nd = &s->node[i];
        nd->sim = s;
        nd->idx = i;
        nd->up = true;
        nd->cfg = cfg->node_template;
        if (!nd->cfg.raps_vid)
            nd->cfg.raps_vid = 100;
        snprintf(nd->cfg.name, sizeof nd->cfg.name, "n%u", i);
        uint8_t id[6] = { 0x02, 0x00, 0x00, 0x00, (uint8_t)(i >> 8), (uint8_t)(i + 1) };
        memcpy(nd->cfg.node_id, id, 6);
        for (int p = 0; p < 2; p++) {
            nd->cfg.port[p].handle = (uint32_t)p;
            snprintf(nd->cfg.port[p].name, sizeof nd->cfg.port[p].name, "n%u.p%d", i, p);
        }
        nd->cfg.sw_raps_forwarding = cfg->sw_forwarding;
        nd->cfg.role = ERPS_ROLE_NONE;
        if (i == owner) {
            nd->cfg.role = ERPS_ROLE_RPL_OWNER;
            nd->cfg.rpl_port = ERPS_PORT1;
        } else if (cfg->neighbour && i == neigh) {
            nd->cfg.role = ERPS_ROLE_RPL_NEIGHBOUR;
            nd->cfg.rpl_port = ERPS_PORT0;
        }
        s->link[i].up[0] = s->link[i].up[1] = true;
    }
    return s;
}

void erps_sim_destroy(erps_sim_t *s)
{
    if (!s)
        return;
    erps_sim_pcap_close(s);
    if (s->node)
        for (unsigned i = 0; i < s->n; i++)
            erps_destroy(s->node[i].e);
    free(s->node);
    free(s->link);
    free(s->heap);
    free(s);
}

erps_config_t *erps_sim_node_config(erps_sim_t *s, unsigned n)
{
    return n < s->n ? &s->node[n].cfg : NULL;
}

static erps_err_t start_node(erps_sim_t *s, struct sim_node *nd)
{
    erps_err_t rc;
    if (!nd->e) {
        char why[160];
        rc = erps_config_validate(&nd->cfg, why, sizeof why);
        if (rc != ERPS_OK) {
            simlog(s, "n%u: invalid config: %s", nd->idx, why);
            return rc;
        }
        rc = erps_create(&nd->cfg, &sim_ops, nd, &nd->e);
        if (rc != ERPS_OK)
            return rc;
    }
    nd->ssf_reported[0] = nd->ssf_reported[1] = false;
    for (int p = 0; p < 2; p++) {
        bool ssf = ssf_of(s, nd->idx, (erps_port_t)p);
        erps_set_signal_fail(nd->e, (erps_port_t)p, ssf, s->now);
        nd->ssf_reported[p] = ssf;
    }
    rc = erps_start(nd->e, s->now);
    nd->started = rc == ERPS_OK;
    return rc;
}

erps_err_t erps_sim_start(erps_sim_t *s)
{
    for (unsigned i = 0; i < s->n; i++) {
        erps_err_t rc = start_node(s, &s->node[i]);
        if (rc != ERPS_OK)
            return rc;
    }
    s->last_change = s->now;
    return ERPS_OK;
}

unsigned erps_sim_nodes(const erps_sim_t *s)
{
    return s->n;
}

erps_t *erps_sim_node(erps_sim_t *s, unsigned n)
{
    return n < s->n ? s->node[n].e : NULL;
}

erps_time_t erps_sim_now(const erps_sim_t *s)
{
    return s->now;
}

/* ------------------------------------------------------------------------------------------ */
/* Invariant                                                                                  */
/* ------------------------------------------------------------------------------------------ */

static bool forwards(const erps_sim_t *s, unsigned link, int dir)
{
    unsigned a = link, b = (link + 1) % s->n;
    const struct sim_node *na = &s->node[a], *nb = &s->node[b];
    if (!s->link[link].up[dir] || !na->up || !nb->up)
        return false;
    return !na->blocked[ERPS_PORT1] && !nb->blocked[ERPS_PORT0];
}

static void check_loops(erps_sim_t *s)
{
    for (int dir = 0; dir < 2; dir++) {
        bool loop = true;
        for (unsigned l = 0; l < s->n && loop; l++)
            loop = forwards(s, l, dir);
        if (loop) {
            if (s->violations++ == 0) {
                char d[200];
                erps_sim_describe(s, d, sizeof d);
                snprintf(s->first_violation, sizeof s->first_violation, "%s loop: %s",
                         dir == 0 ? "CW" : "CCW", d);
            }
            simlog(s, "LOOP (%s) detected", dir == 0 ? "CW" : "CCW");
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Run loop                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static erps_time_t next_time(const erps_sim_t *s)
{
    erps_time_t t = s->heap_len ? s->heap[0].t : ERPS_TIME_NEVER;
    for (unsigned i = 0; i < s->n; i++) {
        const struct sim_node *nd = &s->node[i];
        if (nd->up && nd->started) {
            erps_time_t d = erps_next_deadline(nd->e);
            if (d < t)
                t = d;
        }
    }
    return t;
}

static void handle(erps_sim_t *s, const struct sim_ev *ev)
{
    struct sim_node *nd = &s->node[ev->node];
    if (!nd->up || !nd->started)
        return;
    if (ev->type == EV_FRAME) {
        s->delivered++;
        erps_rx_frame(nd->e, ev->port, ev->frame, ev->len, s->now);
    } else {
        bool ssf = ssf_of(s, ev->node, ev->port);
        if (ssf != nd->ssf_reported[ev->port]) {
            nd->ssf_reported[ev->port] = ssf;
            erps_set_signal_fail(nd->e, ev->port, ssf, s->now);
        }
    }
}

void erps_sim_run_until(erps_sim_t *s, erps_time_t t_end)
{
    for (;;) {
        erps_time_t t = next_time(s);
        if (t == ERPS_TIME_NEVER || t > t_end)
            break;
        if (t > s->now)
            s->now = t;
        while (s->heap_len && s->heap[0].t <= s->now) {
            struct sim_ev ev;
            heap_pop(s, &ev);
            handle(s, &ev);
        }
        for (unsigned i = 0; i < s->n; i++) {
            struct sim_node *nd = &s->node[i];
            if (nd->up && nd->started && erps_next_deadline(nd->e) <= s->now)
                erps_poll(nd->e, s->now);
        }
        check_loops(s);
    }
    if (t_end > s->now)
        s->now = t_end;
}

void erps_sim_run_for(erps_sim_t *s, erps_time_t dt)
{
    erps_sim_run_until(s, s->now + dt);
}

/* ------------------------------------------------------------------------------------------ */
/* Controls                                                                                   */
/* ------------------------------------------------------------------------------------------ */

void erps_sim_link_set(erps_sim_t *s, unsigned link, erps_sim_dir_t dir, bool up)
{
    if (link >= s->n)
        return;
    if (dir & ERPS_SIM_CW)
        s->link[link].up[0] = up;
    if (dir & ERPS_SIM_CCW)
        s->link[link].up[1] = up;
    simlog(s, "link %u %s %s", link,
           dir == ERPS_SIM_BOTH ? "both" : dir == ERPS_SIM_CW ? "cw" : "ccw", up ? "up" : "down");
    schedule_ssf_checks(s);
    check_loops(s);
}

bool erps_sim_link_up(const erps_sim_t *s, unsigned link, erps_sim_dir_t dir)
{
    if (link >= s->n)
        return false;
    bool up = true;
    if (dir & ERPS_SIM_CW)
        up = up && s->link[link].up[0];
    if (dir & ERPS_SIM_CCW)
        up = up && s->link[link].up[1];
    return up;
}

void erps_sim_node_set_up(erps_sim_t *s, unsigned n, bool up)
{
    if (n >= s->n || s->node[n].up == up)
        return;
    struct sim_node *nd = &s->node[n];
    nd->up = up;
    simlog(s, "node %u %s", n, up ? "up" : "down");
    if (!up) {
        if (nd->e && nd->started)
            erps_stop(nd->e, s->now, false);
        nd->started = false;
    } else {
        nd->blocked[0] = nd->blocked[1] = true;
        start_node(s, nd);
    }
    s->last_change = s->now;
    schedule_ssf_checks(s);
    check_loops(s);
}

erps_err_t erps_sim_command(erps_sim_t *s, unsigned n, erps_cmd_t cmd, erps_port_t port,
                            erps_reject_t *why)
{
    if (n >= s->n || !s->node[n].e)
        return ERPS_E_INVAL;
    simlog(s, "n%u command %s%s", n, erps_cmd_str(cmd),
           cmd == ERPS_CMD_CLEAR ? "" : (port == ERPS_PORT0 ? " port 0" : " port 1"));
    erps_err_t rc = erps_command(s->node[n].e, cmd, port, s->now, why);
    check_loops(s);
    return rc;
}

bool erps_sim_port_blocked(const erps_sim_t *s, unsigned n, erps_port_t p)
{
    return n < s->n && p <= ERPS_PORT1 && s->node[n].blocked[p];
}

erps_state_t erps_sim_node_state(const erps_sim_t *s, unsigned n)
{
    if (n >= s->n || !s->node[n].e || !s->node[n].up)
        return ERPS_STATE_INIT;
    erps_status_t st;
    erps_get_status(s->node[n].e, &st);
    return st.state;
}

uint64_t erps_sim_node_flushes(const erps_sim_t *s, unsigned n)
{
    return n < s->n ? s->node[n].flushes : 0;
}

uint64_t erps_sim_frames_delivered(const erps_sim_t *s)
{
    return s->delivered;
}

unsigned erps_sim_loop_violations(const erps_sim_t *s)
{
    return s->violations;
}

const char *erps_sim_first_violation(const erps_sim_t *s)
{
    return s->first_violation;
}

erps_time_t erps_sim_last_change(const erps_sim_t *s)
{
    return s->last_change;
}

int erps_sim_describe(const erps_sim_t *s, char *buf, size_t len)
{
    size_t pos = 0;
#define APP(...)                                                                             \
    do {                                                                                     \
        int k_ = snprintf(pos < len ? buf + pos : NULL, pos < len ? len - pos : 0,           \
                          __VA_ARGS__);                                                      \
        if (k_ > 0)                                                                          \
            pos += (size_t)k_;                                                               \
    } while (0)
    if (len)
        buf[0] = '\0';
    APP("t=%llu.%06llu", (unsigned long long)(s->now / 1000000u),
        (unsigned long long)(s->now % 1000000u));
    for (unsigned i = 0; i < s->n; i++) {
        const struct sim_node *nd = &s->node[i];
        if (!nd->up) {
            APP(" n%u:DOWN", i);
            continue;
        }
        APP(" n%u:%c[%c%c]", i, erps_state_letter(erps_sim_node_state(s, i)),
            nd->blocked[0] ? 'B' : '-', nd->blocked[1] ? 'B' : '-');
    }
    bool any = false;
    for (unsigned l = 0; l < s->n; l++) {
        const struct sim_link *k = &s->link[l];
        if (!k->up[0] || !k->up[1]) {
            APP("%s%u:%s", any ? "," : " links-down ", l,
                (!k->up[0] && !k->up[1]) ? "x" : (!k->up[0] ? "cw" : "ccw"));
            any = true;
        }
    }
#undef APP
    return (int)pos;
}

void erps_sim_set_log(erps_sim_t *s, FILE *out, erps_log_level_t level)
{
    s->log = out;
    s->log_level = level;
    for (unsigned i = 0; i < s->n; i++) {
        s->node[i].cfg.log_level = level;
        if (s->node[i].e)
            erps_set_log_level(s->node[i].e, out ? level : ERPS_LOG_ERR);
    }
}

void erps_sim_set_event_hook(erps_sim_t *s, erps_sim_event_fn fn, void *ctx)
{
    s->hook = fn;
    s->hook_ctx = ctx;
}
