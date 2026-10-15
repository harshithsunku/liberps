/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Flush logic (§10.1.10) and FDB flush coalescing (§9.6).
 *
 * For each ring port the last (node ID, BPR) pair received is kept. A new pair received on a
 * port that differs from the pair stored for that port and from the pair stored for the other
 * port means the blocked link of the ring moved: the FDB must be flushed — unless the message
 * carries DNF or is our own. R-APS(NR) (without RB) deletes the stored pair of its port without
 * storing a new one. Blocking a ring port deletes both pairs.
 *
 * Several flush triggers during one input (e.g. a row's "Flush FDB" and the flush logic for the
 * same message) are coalesced into a single flush_fdb() call when the input completes, after all
 * port state changes have been applied.
 */
#include "erps_internal.h"

#include <string.h>

static bool pair_equal(const struct erps_port *pp, const erps_raps_msg_t *m)
{
    return pp->pair_valid && pp->pair_bpr == m->bpr && memcmp(pp->pair_node, m->node_id, 6) == 0;
}

void erps__flush_rx(erps_t *e, erps_port_t p, const erps_raps_msg_t *m)
{
    struct erps_port *pp = &e->port[p];
    struct erps_port *po = &e->port[erps__other(p)];

    if (m->req == ERPS_RAPS_NR && !m->rb) {
        pp->pair_valid = false; /* deleted, not replaced */
        return;
    }
    if (pair_equal(pp, m))
        return;

    pp->pair_valid = true;
    memcpy(pp->pair_node, m->node_id, 6);
    pp->pair_bpr = m->bpr;

    if (pair_equal(po, m))
        return;
    if (m->dnf || memcmp(m->node_id, e->cfg.node_id, 6) == 0)
        return;

    if (e->ops.log && e->cfg.log_level >= ERPS_LOG_DEBUG) {
        char b[24];
        erps__fmt_mac(m->node_id, b, sizeof b);
        erps__log(e, ERPS_LOG_DEBUG, "flush logic: new (%s, %u) on port %d", b, m->bpr, (int)p);
    }
    erps__flush_request(e);
}

void erps__flush_clear_pairs(erps_t *e)
{
    e->port[0].pair_valid = false;
    e->port[1].pair_valid = false;
}

void erps__flush_request(erps_t *e)
{
    e->ctr.flush_requests++;
    e->flush_pending = true;
}

void erps__flush_commit(erps_t *e)
{
    if (!e->flush_pending)
        return;
    e->flush_pending = false;
    int rc = e->ops.flush_fdb(e->user);
    if (rc) {
        erps__platform_error(e, "flush_fdb", rc, ERPS_PORT_NONE);
        return;
    }
    e->ctr.flushes++;
    erps__log(e, ERPS_LOG_INFO, "FDB flushed");
    erps_event_t ev = { .type = ERPS_EV_FLUSH };
    erps__notify(e, &ev);
}
