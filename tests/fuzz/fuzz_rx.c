/* SPDX-License-Identifier: Apache-2.0 */

#include "erps/erps.h"
#include "erps/erps_pdu.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct harness {
    bool port_blocked[2];
    bool port_desynced[2];
    int fail_set_port;
    int fail_flush;
    int fail_tx;
    unsigned failures_injected;
    erps_counters_t prev_counters;
    bool cleared_counters;
    uint32_t tx_count;
    uint32_t flush_count;
    uint32_t notify_count;
    erps_event_t last_event;
};

static int harness_tx(void *user, erps_port_t port, const uint8_t *frame, size_t len)
{
    struct harness *h = user;
    (void)port;
    (void)frame;
    (void)len;
    if (h->fail_tx > 0) {
        h->fail_tx--;
        h->failures_injected++;
        return -1;
    }
    h->tx_count++;
    return 0;
}

static int harness_set_port_state(void *user, erps_port_t port, unsigned mask)
{
    struct harness *h = user;
    if (port != ERPS_PORT0 && port != ERPS_PORT1)
        return -1;
    if (h->fail_set_port > 0) {
        h->fail_set_port--;
        h->failures_injected++;
        h->port_desynced[port] = true;
        return -1;
    }
    h->port_blocked[port] = (mask != 0);
    h->port_desynced[port] = false;
    return 0;
}

static int harness_flush_fdb(void *user)
{
    struct harness *h = user;
    if (h->fail_flush > 0) {
        h->fail_flush--;
        h->failures_injected++;
        return -1;
    }
    h->flush_count++;
    return 0;
}

static void harness_notify(void *user, const erps_event_t *ev)
{
    struct harness *h = user;
    h->notify_count++;
    if (ev)
        h->last_event = *ev;
}

static void harness_log(void *user, erps_log_level_t level, const char *msg)
{
    (void)user;
    (void)level;
    (void)msg;
}

static const erps_ops_t harness_ops = {
    .tx = harness_tx,
    .set_port_state = harness_set_port_state,
    .flush_fdb = harness_flush_fdb,
    .notify = harness_notify,
    .log = harness_log,
};

static void assert_invariants(erps_t *e, struct harness *h)
{
    erps_status_t st;
    if (erps_get_status(e, &st) != ERPS_OK)
        abort();

    /* State is a valid enum */
    if (st.state < ERPS_STATE_INIT || st.state >= ERPS_STATE_COUNT)
        abort();

    /* Port blocked flags equal what harness set_port_state recorded */
    if (h->failures_injected == 0) {
        if (st.port[0].blocked != h->port_blocked[0] ||
            st.port[1].blocked != h->port_blocked[1])
            abort();
    } else {
        if (!h->port_desynced[0] && st.port[0].blocked != h->port_blocked[0])
            abort();
        if (!h->port_desynced[1] && st.port[1].blocked != h->port_blocked[1])
            abort();
    }

    /* History seq strictly increasing */
    erps_trace_t hist[ERPS_HISTORY_MAX];
    size_t nhist = erps_get_history(e, hist, ERPS_HISTORY_MAX);
    for (size_t i = 1; i < nhist; i++) {
        if (hist[i].seq <= hist[i - 1].seq)
            abort();
    }

    /* Counters never decrease (except after clear) */
    erps_counters_t ct;
    if (erps_get_counters(e, &ct) != ERPS_OK)
        abort();

    if (!h->cleared_counters) {
        const uint64_t *curr = (const uint64_t *)&ct;
        const uint64_t *prev = (const uint64_t *)&h->prev_counters;
        size_t n = sizeof(erps_counters_t) / sizeof(uint64_t);
        for (size_t i = 0; i < n; i++) {
            if (curr[i] < prev[i])
                abort();
        }
    }
    h->prev_counters = ct;
    h->cleared_counters = false;

    /* erps_dump text */
    int need_txt = erps_dump(e, ERPS_DUMP_ALL, ERPS_FMT_TEXT, NULL, 0);
    if (need_txt < 0)
        abort();
    char *buf_txt = malloc((size_t)need_txt + 1);
    if (!buf_txt)
        abort();
    int wrote_txt = erps_dump(e, ERPS_DUMP_ALL, ERPS_FMT_TEXT, buf_txt, (size_t)need_txt + 1);
    if (wrote_txt != need_txt || strlen(buf_txt) > (size_t)need_txt)
        abort();
    free(buf_txt);

    /* erps_dump JSON */
    int need_json = erps_dump(e, ERPS_DUMP_ALL, ERPS_FMT_JSON, NULL, 0);
    if (need_json < 0)
        abort();
    char *buf_json = malloc((size_t)need_json + 1);
    if (!buf_json)
        abort();
    int wrote_json = erps_dump(e, ERPS_DUMP_ALL, ERPS_FMT_JSON, buf_json, (size_t)need_json + 1);
    if (wrote_json != need_json || strlen(buf_json) > (size_t)need_json)
        abort();

    /* JSON braces balance */
    int brace = 0;
    int bracket = 0;
    bool in_str = false;
    bool esc = false;
    for (int i = 0; i < wrote_json; i++) {
        char ch = buf_json[i];
        if (esc) {
            esc = false;
            continue;
        }
        if (ch == '\\') {
            esc = true;
            continue;
        }
        if (ch == '"') {
            in_str = !in_str;
            continue;
        }
        if (!in_str) {
            if (ch == '{') {
                brace++;
            } else if (ch == '}') {
                brace--;
                if (brace < 0)
                    abort();
            } else if (ch == '[') {
                bracket++;
            } else if (ch == ']') {
                bracket--;
                if (bracket < 0)
                    abort();
            }
        }
    }
    if (brace != 0 || bracket != 0 || in_str)
        abort();
    free(buf_json);

    /* Test buffer truncation */
    char small[32];
    int tr_txt = erps_dump(e, ERPS_DUMP_ALL, ERPS_FMT_TEXT, small, sizeof small);
    if (tr_txt != need_txt || strlen(small) >= sizeof small)
        abort();
    int tr_json = erps_dump(e, ERPS_DUMP_ALL, ERPS_FMT_JSON, small, sizeof small);
    if (tr_json != need_json || strlen(small) >= sizeof small)
        abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 8)
        return 0;

    /* Instance configuration from the first input bytes */
    static const erps_role_t roles[] = {
        ERPS_ROLE_NONE, ERPS_ROLE_RPL_OWNER, ERPS_ROLE_RPL_NEIGHBOUR
    };
    erps_role_t role = roles[data[0] % 3];
    erps_port_t rpl_port = (data[1] & 1) ? ERPS_PORT1 : ERPS_PORT0;
    bool revertive = (data[2] & 1) != 0;
    uint8_t compat = (data[3] & 1) ? 2 : 1;
    bool relaxed = (data[4] & 1) != 0;

    erps_config_t cfg;
    erps_config_init(&cfg);
    snprintf(cfg.name, sizeof cfg.name, "fz");
    static const uint8_t own_node_id[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x50 };
    memcpy(cfg.node_id, own_node_id, 6);
    cfg.raps_vid = 100;
    cfg.ring_id = 1;
    cfg.mel = 7;
    cfg.role = role;
    cfg.rpl_port = rpl_port;
    cfg.revertive = revertive;
    cfg.compat_version = compat;
    cfg.relaxed_timers = relaxed;
    cfg.sw_raps_forwarding = (data[5] & 1) != 0;
    cfg.fop_pm_enable = (data[6] & 1) != 0;
    cfg.fop_to_enable = (data[7] & 1) != 0;

    struct harness h;
    memset(&h, 0, sizeof h);

    erps_t *e = NULL;
    if (erps_create(&cfg, &harness_ops, &h, &e) != ERPS_OK)
        return 0;

    erps_time_t now = ERPS_SEC(100);
    erps_start(e, now);
    assert_invariants(e, &h);

    size_t pos = 8;
    unsigned op_count = 0;
    const unsigned max_ops = 64;

    while (pos < size && op_count < max_ops) {
        uint8_t op = data[pos++] % 12;
        op_count++;

        switch (op) {
        case 0: { /* Raw frame */
            if (pos >= size)
                break;
            erps_port_t p = (data[pos++] & 1) ? ERPS_PORT1 : ERPS_PORT0;
            if (pos >= size)
                break;
            size_t flen = data[pos++];
            if (pos + flen > size)
                flen = size - pos;
            erps_rx_frame(e, p, data + pos, flen, now);
            pos += flen;
            break;
        }
        case 1: { /* Well-formed R-APS frame */
            if (pos + 6 > size)
                break;
            erps_port_t p = (data[pos++] & 1) ? ERPS_PORT1 : ERPS_PORT0;
            static const uint8_t req_codes[] = {
                ERPS_RAPS_NR, ERPS_RAPS_MS, ERPS_RAPS_SF, ERPS_RAPS_FS, ERPS_RAPS_EVENT, 0x1, 0xF
            };
            uint8_t req = req_codes[data[pos++] % (sizeof(req_codes) / sizeof(req_codes[0]))];
            uint8_t st_flags = data[pos++];
            bool rb = (st_flags & 1) != 0;
            bool dnf = (st_flags & 2) != 0;
            uint8_t bpr = (st_flags & 4) ? 1 : 0;
            uint8_t nid_byte = data[pos++];
            uint8_t node_mac[6] = { 0x02, 0, 0, 0, 0, (uint8_t)(nid_byte ? nid_byte : 0x02) };
            if (nid_byte == 0xFF)
                memcpy(node_mac, own_node_id, 6);

            uint8_t ring_id = (data[pos++] & 1) ? 1 : (uint8_t)(data[pos - 1] % 5 + 1);
            uint8_t mel = (data[pos++] & 1) ? 7 : (uint8_t)(data[pos - 1] & 7);

            erps_frame_params_t fp = {
                .ring_id = ring_id,
                .vid = 100,
                .pcp = 7,
                .mel = mel,
                .version = 1,
            };
            memcpy(fp.src, node_mac, 6);

            erps_raps_msg_t msg;
            memset(&msg, 0, sizeof msg);
            msg.req = req;
            msg.subcode = (req == ERPS_RAPS_EVENT) ? 0 : 0;
            msg.rb = rb;
            msg.dnf = dnf;
            msg.bpr = bpr;
            memcpy(msg.node_id, node_mac, 6);

            uint8_t fbuf[ERPS_FRAME_MAX_LEN];
            size_t n = erps_frame_encode(&fp, &msg, fbuf, sizeof fbuf);
            if (n > 0)
                erps_rx_frame(e, p, fbuf, n, now);
            break;
        }
        case 2: { /* Well-formed R-APS via erps_rx_pdu */
            if (pos + 4 > size)
                break;
            erps_port_t p = (data[pos++] & 1) ? ERPS_PORT1 : ERPS_PORT0;
            uint8_t req = (data[pos++] & 1) ? ERPS_RAPS_SF : ERPS_RAPS_NR;
            uint8_t nid_byte = data[pos++];
            uint8_t node_mac[6] = { 0x02, 0, 0, 0, 0, (uint8_t)(nid_byte ? nid_byte : 0x02) };

            erps_rx_meta_t meta;
            erps_raps_dmac(1, meta.dst);
            memcpy(meta.src, node_mac, 6);
            meta.vid = (data[pos++] & 1) ? 100 : 200;
            meta.pcp = 7;

            erps_raps_msg_t msg;
            memset(&msg, 0, sizeof msg);
            msg.req = req;
            msg.rb = (req == ERPS_RAPS_NR);
            memcpy(msg.node_id, node_mac, 6);

            uint8_t pdu[ERPS_PDU_LEN];
            size_t plen = erps_pdu_encode(7, 1, &msg, pdu, sizeof pdu);
            if (plen > 0)
                erps_rx_pdu(e, p, &meta, pdu, plen, now);
            break;
        }
        case 3: { /* Advance time / poll */
            if (pos >= size)
                break;
            uint8_t mode = data[pos++];
            if (mode & 1) {
                erps_time_t d = erps_next_deadline(e);
                if (d != ERPS_TIME_NEVER && d >= now)
                    now = d;
                else
                    now += ERPS_MSEC(10);
            } else {
                static const erps_time_t dt_tbl[] = {
                    ERPS_USEC(50), ERPS_MSEC(1), ERPS_MSEC(10), ERPS_MSEC(100),
                    ERPS_MSEC(500), ERPS_SEC(1), ERPS_SEC(5), ERPS_SEC(60)
                };
                erps_time_t dt = dt_tbl[mode % (sizeof(dt_tbl) / sizeof(dt_tbl[0]))];
                now += dt;
            }
            erps_poll(e, now);
            break;
        }
        case 4: { /* Signal fail */
            if (pos >= size)
                break;
            uint8_t b = data[pos++];
            erps_port_t p = (b & 1) ? ERPS_PORT1 : ERPS_PORT0;
            bool ssf = (b & 2) != 0;
            erps_set_signal_fail(e, p, ssf, now);
            break;
        }
        case 5: { /* FS/MS/Clear */
            if (pos >= size)
                break;
            uint8_t b = data[pos++];
            erps_cmd_t cmd = (erps_cmd_t)(b % 3);
            erps_port_t p = (b & 4) ? ERPS_PORT1 : ERPS_PORT0;
            erps_reject_t why = ERPS_REJECT_NONE;
            erps_command(e, cmd, p, now, &why);
            break;
        }
        case 6: { /* Flush event */
            erps_send_flush_event(e, now);
            break;
        }
        case 7: { /* Stop / Start */
            erps_status_t cur_st;
            erps_get_status(e, &cur_st);
            if (cur_st.running) {
                bool block = (pos < size) ? ((data[pos++] & 1) != 0) : true;
                erps_stop(e, now, block);
            } else {
                erps_start(e, now);
            }
            break;
        }
        case 8: { /* Change timers */
            if (pos + 3 > size)
                break;
            uint8_t tw = data[pos++];
            uint8_t tg = data[pos++];
            uint8_t th = data[pos++];
            uint32_t wtr = (tw % 12 + 1) * 60000;
            uint32_t guard = (tg % 200 + 1) * 10;
            uint32_t holdoff = (th % 101) * 100;
            erps_set_timers(e, wtr, guard, holdoff);
            break;
        }
        case 9: { /* Change revertive */
            if (pos >= size)
                break;
            bool rev = (data[pos++] & 1) != 0;
            erps_set_revertive(e, rev);
            break;
        }
        case 10: { /* Clear counters */
            erps_clear_counters(e);
            h.cleared_counters = true;
            break;
        }
        case 11: { /* Inject platform callback failures */
            if (pos + 2 > size)
                break;
            uint8_t which = data[pos++];
            int count = (data[pos++] % 2) + 1;
            if (which & 1)
                h.fail_set_port += count;
            if (which & 2)
                h.fail_flush += count;
            if (which & 4)
                h.fail_tx += count;
            break;
        }
        default:
            break;
        }

        assert_invariants(e, &h);
    }

    erps_destroy(e);
    return 0;
}
