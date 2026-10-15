/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Minimal, complete liberps integration: two nodes on a 2-node ring, wired back to back through
 * an in-memory "wire" with 100 us latency, driven by one event loop on a virtual clock.
 * It shows everything a platform port has to provide:
 *
 *   1. erps_ops_t callbacks: tx, set_port_state, flush_fdb (+ optional notify, log);
 *   2. inputs: erps_rx_frame() for received R-APS, erps_set_signal_fail() for link state,
 *      erps_command() for operator commands;
 *   3. time: call erps_poll() when erps_next_deadline() is reached.
 *
 * Build: make examples   Run: build/<cc>/two_nodes
 */
#include <erps/erps.h>

#include <stdio.h>
#include <string.h>

#define WIRE_DELAY ERPS_USEC(100)
#define MAXQ 64

struct node;

struct frame {
    erps_time_t at;
    struct node *to;
    erps_port_t port;
    size_t len;
    uint8_t data[64];
};

static struct frame queue[MAXQ];
static size_t qlen;
static erps_time_t now;

struct node {
    const char *name;
    erps_t *erps;
    struct node *peer[2]; /* who is at the other end of my port 0 / port 1 */
    erps_port_t peer_port[2];
    bool link_up[2];
    bool blocked[2]; /* the "dataplane" */
};

/* ---- the platform: three required callbacks ------------------------------------------- */

static int my_tx(void *user, erps_port_t port, const uint8_t *frame, size_t len)
{
    struct node *n = user;
    if (!n->link_up[port] || qlen == MAXQ || len > sizeof queue[0].data)
        return 0; /* frame lost on a dead link */
    struct frame *f = &queue[qlen++];
    f->at = now + WIRE_DELAY;
    f->to = n->peer[port];
    f->port = n->peer_port[port];
    f->len = len;
    memcpy(f->data, frame, len);
    return 0;
}

static int my_set_port_state(void *user, erps_port_t port, unsigned block_mask)
{
    struct node *n = user;
    n->blocked[port] = block_mask != 0; /* real code: VLAN membership / STP state in hardware */
    return 0;
}

static int my_flush_fdb(void *user)
{
    struct node *n = user;
    printf("    %s: flush FDB\n", n->name); /* real code: flush MACs on both ring ports */
    return 0;
}

static void my_notify(void *user, const erps_event_t *ev)
{
    struct node *n = user;
    if (ev->type == ERPS_EV_STATE_CHANGE)
        printf("    %s: %s -> %s (Table 10-2 row %u)\n", n->name, erps_state_str(ev->u.state.from),
               erps_state_str(ev->u.state.to), ev->u.state.row);
}

static const erps_ops_t ops = {
    .tx = my_tx,
    .set_port_state = my_set_port_state,
    .flush_fdb = my_flush_fdb,
    .notify = my_notify,
};

/* ---- the event loop ------------------------------------------------------------------ */

static void run_until(struct node **nodes, size_t nn, erps_time_t end)
{
    for (;;) {
        erps_time_t next = end;
        for (size_t i = 0; i < qlen; i++)
            if (queue[i].at < next)
                next = queue[i].at;
        for (size_t i = 0; i < nn; i++) {
            erps_time_t d = erps_next_deadline(nodes[i]->erps);
            if (d < next)
                next = d;
        }
        if (next >= end)
            break;
        now = next;
        for (size_t i = 0; i < qlen;) { /* deliver due frames */
            if (queue[i].at <= now) {
                struct frame f = queue[i];
                queue[i] = queue[--qlen];
                erps_rx_frame(f.to->erps, f.port, f.data, f.len, now);
            } else {
                i++;
            }
        }
        for (size_t i = 0; i < nn; i++)
            if (erps_next_deadline(nodes[i]->erps) <= now)
                erps_poll(nodes[i]->erps, now);
    }
    now = end;
}

static erps_state_t state_of(const struct node *n)
{
    erps_status_t st;
    erps_get_status(n->erps, &st);
    return st.state;
}

static void show(struct node **nodes, size_t nn)
{
    printf("  t=%7.3fs", (double)now / 1e6);
    for (size_t i = 0; i < nn; i++)
        printf("  %s:%-13s p0 %s p1 %s", nodes[i]->name, erps_state_str(state_of(nodes[i])),
               nodes[i]->blocked[0] ? "BLK" : "fwd", nodes[i]->blocked[1] ? "BLK" : "fwd");
    printf("\n");
}

static void set_link(struct node *a, erps_port_t pa, bool up)
{
    struct node *b = a->peer[pa];
    erps_port_t pb = a->peer_port[pa];
    a->link_up[pa] = b->link_up[pb] = up;
    erps_set_signal_fail(a->erps, pa, !up, now); /* real code: carrier / CCM loss */
    erps_set_signal_fail(b->erps, pb, !up, now);
}

int main(void)
{
    struct node A = { .name = "A" }, B = { .name = "B" };
    /* link 0: A.p1 <-> B.p0, link 1 (the RPL): B.p1 <-> A.p0 */
    A.peer[1] = &B, A.peer_port[1] = ERPS_PORT0;
    B.peer[0] = &A, B.peer_port[0] = ERPS_PORT1;
    B.peer[1] = &A, B.peer_port[1] = ERPS_PORT0;
    A.peer[0] = &B, A.peer_port[0] = ERPS_PORT1;
    A.link_up[0] = A.link_up[1] = B.link_up[0] = B.link_up[1] = true;
    struct node *nodes[2] = { &A, &B };

    for (int i = 0; i < 2; i++) {
        erps_config_t cfg;
        erps_config_init(&cfg);
        snprintf(cfg.name, sizeof cfg.name, "%s", nodes[i]->name);
        cfg.raps_vid = 100;
        cfg.node_id[0] = 0x02;
        cfg.node_id[5] = (uint8_t)(i + 1);
        cfg.relaxed_timers = true; /* short WTR for the demo */
        cfg.wtr_ms = 10000;
        if (i == 0) {
            cfg.role = ERPS_ROLE_RPL_OWNER; /* A owns the RPL on its port 0 */
            cfg.rpl_port = ERPS_PORT0;
        } else {
            cfg.role = ERPS_ROLE_RPL_NEIGHBOUR;
            cfg.rpl_port = ERPS_PORT1;
        }
        if (erps_create(&cfg, &ops, nodes[i], &nodes[i]->erps) != ERPS_OK)
            return 1;
        erps_start(nodes[i]->erps, now);
    }

    printf("start (owner waits WTR before Idle):\n");
    run_until(nodes, 2, ERPS_SEC(11));
    show(nodes, 2);

    printf("link 0 (A.p1-B.p0) fails:\n");
    set_link(&A, ERPS_PORT1, false);
    run_until(nodes, 2, now + ERPS_SEC(1));
    show(nodes, 2);

    printf("link 0 recovers (revertive: WTR 10 s):\n");
    set_link(&A, ERPS_PORT1, true);
    run_until(nodes, 2, now + ERPS_SEC(12));
    show(nodes, 2);

    printf("forced switch on B port 0, then clear:\n");
    erps_command(B.erps, ERPS_CMD_FORCED_SWITCH, ERPS_PORT0, now, NULL);
    run_until(nodes, 2, now + ERPS_SEC(1));
    show(nodes, 2);
    erps_command(B.erps, ERPS_CMD_CLEAR, ERPS_PORT0, now, NULL);
    run_until(nodes, 2, now + ERPS_SEC(7)); /* WTB = guard + 5 s */
    show(nodes, 2);

    erps_destroy(A.erps);
    erps_destroy(B.erps);
    return 0;
}
