/* SPDX-License-Identifier: Apache-2.0 */
/* Instance manager — see include/erps/erps_mgr.h. */
#include "erps/erps_mgr.h"
#include "erps_internal.h"

#include <stdlib.h>
#include <string.h>

struct erps_mgr {
    erps_t **inst;
    size_t n, cap;
};

erps_mgr_t *erps_mgr_create(void)
{
    return calloc(1, sizeof(struct erps_mgr));
}

void erps_mgr_destroy(erps_mgr_t *m)
{
    if (!m)
        return;
    for (size_t i = 0; i < m->n; i++)
        erps_destroy(m->inst[i]);
    free(m->inst);
    free(m);
}

static bool shares_port(const erps_config_t *a, const erps_config_t *b)
{
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            if (a->port[i].handle == b->port[j].handle)
                return true;
    return false;
}

erps_err_t erps_mgr_add(erps_mgr_t *m, const erps_config_t *cfg, const erps_ops_t *ops,
                        void *user, erps_t **out)
{
    if (!m || !cfg || !out)
        return ERPS_E_INVAL;
    if (!memchr(cfg->name, '\0', sizeof cfg->name) || !cfg->name[0])
        return ERPS_E_CONFIG;
    if (cfg->port[0].handle == cfg->port[1].handle)
        return ERPS_E_CONFIG;
    for (size_t i = 0; i < m->n; i++) {
        const erps_config_t *c = &m->inst[i]->cfg;
        if (strcmp(c->name, cfg->name) == 0)
            return ERPS_E_EXISTS;
        if (shares_port(c, cfg) && c->ring_id == cfg->ring_id && c->raps_vid == cfg->raps_vid)
            return ERPS_E_EXISTS;
    }
    if (m->n == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 4;
        erps_t **p = realloc(m->inst, cap * sizeof *p);
        if (!p)
            return ERPS_E_NOMEM;
        m->inst = p;
        m->cap = cap;
    }
    erps_err_t rc = erps_create(cfg, ops, user, out);
    if (rc != ERPS_OK)
        return rc;
    m->inst[m->n++] = *out;
    return ERPS_OK;
}

erps_err_t erps_mgr_remove(erps_mgr_t *m, erps_t *e)
{
    if (!m || !e)
        return ERPS_E_INVAL;
    for (size_t i = 0; i < m->n; i++) {
        if (m->inst[i] == e) {
            erps_destroy(e);
            memmove(&m->inst[i], &m->inst[i + 1], (m->n - i - 1) * sizeof *m->inst);
            m->n--;
            return ERPS_OK;
        }
    }
    return ERPS_E_NOTFOUND;
}

erps_t *erps_mgr_find(const erps_mgr_t *m, const char *name)
{
    if (!m || !name)
        return NULL;
    for (size_t i = 0; i < m->n; i++)
        if (strcmp(m->inst[i]->cfg.name, name) == 0)
            return m->inst[i];
    return NULL;
}

size_t erps_mgr_count(const erps_mgr_t *m)
{
    return m ? m->n : 0;
}

erps_t *erps_mgr_at(const erps_mgr_t *m, size_t i)
{
    return (m && i < m->n) ? m->inst[i] : NULL;
}

erps_err_t erps_mgr_rx_frame(erps_mgr_t *m, uint32_t handle, const uint8_t *frame, size_t len,
                             erps_time_t now)
{
    if (!m || (!frame && len))
        return ERPS_E_INVAL;
    erps_eth_t eth;
    uint8_t ring_id;
    if (erps_eth_parse(frame, len, &eth) != ERPS_DROP_NONE || !eth.tagged ||
        !erps_is_raps_dmac(eth.dst, &ring_id))
        return ERPS_E_NOTFOUND;
    for (size_t i = 0; i < m->n; i++) {
        erps_t *e = m->inst[i];
        if (e->cfg.raps_vid != eth.vid || e->cfg.ring_id != ring_id)
            continue;
        for (int p = 0; p < 2; p++)
            if (e->cfg.port[p].handle == handle)
                return erps_rx_frame(e, (erps_port_t)p, frame, len, now);
    }
    return ERPS_E_NOTFOUND;
}

erps_err_t erps_mgr_set_signal_fail(erps_mgr_t *m, uint32_t handle, bool ssf, erps_time_t now)
{
    if (!m)
        return ERPS_E_INVAL;
    erps_err_t rc = ERPS_E_NOTFOUND;
    for (size_t i = 0; i < m->n; i++) {
        erps_t *e = m->inst[i];
        for (int p = 0; p < 2; p++) {
            if (e->cfg.port[p].handle == handle) {
                erps_err_t r = erps_set_signal_fail(e, (erps_port_t)p, ssf, now);
                if (rc == ERPS_E_NOTFOUND || r != ERPS_OK)
                    rc = r;
            }
        }
    }
    return rc;
}

erps_err_t erps_mgr_poll(erps_mgr_t *m, erps_time_t now)
{
    if (!m)
        return ERPS_E_INVAL;
    for (size_t i = 0; i < m->n; i++)
        if (erps_next_deadline(m->inst[i]) <= now)
            erps_poll(m->inst[i], now);
    return ERPS_OK;
}

erps_time_t erps_mgr_next_deadline(const erps_mgr_t *m)
{
    erps_time_t t = ERPS_TIME_NEVER;
    if (!m)
        return t;
    for (size_t i = 0; i < m->n; i++) {
        erps_time_t d = erps_next_deadline(m->inst[i]);
        if (d < t)
            t = d;
    }
    return t;
}
