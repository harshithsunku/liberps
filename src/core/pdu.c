/* SPDX-License-Identifier: Apache-2.0 */

#include "erps/erps_pdu.h"

#include <string.h>

const uint8_t erps_raps_dmac_prefix[5] = {0x01, 0x19, 0xA7, 0x00, 0x00};

void erps_raps_dmac(uint8_t ring_id, uint8_t out[6])
{
    memcpy(out, erps_raps_dmac_prefix, sizeof(erps_raps_dmac_prefix));
    out[5] = ring_id;
}

bool erps_is_raps_dmac(const uint8_t dst[6], uint8_t *ring_id)
{
    if (memcmp(dst, erps_raps_dmac_prefix, sizeof(erps_raps_dmac_prefix)) != 0)
        return false;

    if (ring_id != NULL)
        *ring_id = dst[5];
    return true;
}

erps_drop_t erps_eth_parse(const uint8_t *frame, size_t len, erps_eth_t *eth)
{
    erps_eth_t parsed = {0};
    size_t offset = ERPS_ETH_HDR_LEN;
    uint16_t type;
    unsigned tags = 0;

    if (len < ERPS_ETH_HDR_LEN)
        return ERPS_DROP_SHORT;

    memcpy(parsed.dst, frame, sizeof(parsed.dst));
    memcpy(parsed.src, frame + 6, sizeof(parsed.src));
    type = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);

    while ((type == ERPS_ETHERTYPE_VLAN || type == ERPS_ETHERTYPE_QINQ) && tags < 2u) {
        uint16_t tci;

        if (len - offset < ERPS_VLAN_TAG_LEN)
            return ERPS_DROP_SHORT;
        tci = (uint16_t)(((uint16_t)frame[offset] << 8) | frame[offset + 1]);
        if (tags == 0u) {
            parsed.tagged = true;
            parsed.pcp = (uint8_t)(tci >> 13);
            parsed.vid = (uint16_t)(tci & 0x0FFFu);
        }
        type = (uint16_t)(((uint16_t)frame[offset + 2] << 8) | frame[offset + 3]);
        offset += ERPS_VLAN_TAG_LEN;
        ++tags;
    }

    parsed.ethertype = type;
    parsed.hdr_len = offset;
    *eth = parsed;
    return ERPS_DROP_NONE;
}

erps_drop_t erps_pdu_decode(const uint8_t *pdu, size_t len, erps_pdu_t *out)
{
    uint8_t status;

    /* A truncated common header leaves the caller's output untouched. */
    if (len < ERPS_PDU_HDR_LEN)
        return ERPS_DROP_SHORT;

    memset(out, 0, sizeof(*out));
    out->mel = (uint8_t)(pdu[0] >> 5);
    out->version = (uint8_t)(pdu[0] & 0x1Fu);
    out->opcode = pdu[1];
    out->flags = pdu[2];
    out->tlv_offset = pdu[3];

    if (out->opcode != ERPS_OPCODE_RAPS)
        return ERPS_DROP_OPCODE;
    if (out->tlv_offset < ERPS_TLV_OFFSET)
        return ERPS_DROP_TLV_OFFSET;
    if (len < ERPS_PDU_HDR_LEN + ERPS_RAPS_INFO_LEN)
        return ERPS_DROP_SHORT;

    /* G.8032 §10.3: the first 12 octets carry the information; Reserved2 is ignored. */
    out->msg.req = (uint8_t)(pdu[4] >> 4);
    out->msg.subcode = (uint8_t)(pdu[4] & 0x0Fu);
    status = pdu[5];
    out->msg.status = status;
    out->msg.rb = (status & ERPS_STATUS_RB) != 0u;
    out->msg.dnf = (status & ERPS_STATUS_DNF) != 0u;
    out->msg.bpr = (uint8_t)((status & ERPS_STATUS_BPR) != 0u);
    memcpy(out->msg.node_id, pdu + 6, sizeof(out->msg.node_id));

    /* G.8032 §10.1.6: only these Request/State values are assigned. */
    switch (out->msg.req) {
    case ERPS_RAPS_NR:
    case ERPS_RAPS_MS:
    case ERPS_RAPS_SF:
    case ERPS_RAPS_FS:
        return ERPS_DROP_NONE;
    case ERPS_RAPS_EVENT:
        if (out->msg.subcode != 0u || status != 0u)
            return ERPS_DROP_EVENT_SUBCODE;
        return ERPS_DROP_NONE;
    default:
        return ERPS_DROP_REQ_RESERVED;
    }
}

uint8_t erps_status_octet(bool rb, bool dnf, uint8_t bpr)
{
    return (uint8_t)((rb ? ERPS_STATUS_RB : 0u) |
                     (dnf ? ERPS_STATUS_DNF : 0u) |
                     (bpr != 0u ? ERPS_STATUS_BPR : 0u));
}

size_t erps_pdu_encode(uint8_t mel, uint8_t version, const erps_raps_msg_t *msg,
                       uint8_t *buf, size_t len)
{
    if (len < ERPS_PDU_LEN)
        return 0;

    memset(buf, 0, ERPS_PDU_LEN);
    buf[0] = (uint8_t)(((mel & 0x07u) << 5) | (version & 0x1Fu));
    buf[1] = ERPS_OPCODE_RAPS;
    buf[3] = ERPS_TLV_OFFSET;
    buf[4] = (uint8_t)(((msg->req & 0x0Fu) << 4) |
                       (msg->req == ERPS_RAPS_EVENT ? (msg->subcode & 0x0Fu) : 0u));
    buf[5] = erps_status_octet(msg->rb, msg->dnf, msg->bpr);
    memcpy(buf + 6, msg->node_id, sizeof(msg->node_id));
    return ERPS_PDU_LEN;
}

size_t erps_frame_encode(const erps_frame_params_t *p, const erps_raps_msg_t *msg,
                         uint8_t *buf, size_t len)
{
    if (len < ERPS_FRAME_MIN_LEN)
        return 0;

    memset(buf, 0, ERPS_FRAME_MIN_LEN);
    erps_raps_dmac(p->ring_id, buf);
    memcpy(buf + 6, p->src, sizeof(p->src));
    buf[12] = (uint8_t)(ERPS_ETHERTYPE_VLAN >> 8);
    buf[13] = (uint8_t)ERPS_ETHERTYPE_VLAN;
    {
        uint16_t tci = (uint16_t)((((uint16_t)p->pcp & 0x07u) << 13) |
                                  (p->vid & 0x0FFFu));

        buf[14] = (uint8_t)(tci >> 8);
        buf[15] = (uint8_t)tci;
    }
    buf[16] = (uint8_t)(ERPS_ETHERTYPE_CFM >> 8);
    buf[17] = (uint8_t)ERPS_ETHERTYPE_CFM;
    (void)erps_pdu_encode(p->mel, p->version, msg, buf + 18, len - 18);
    return ERPS_FRAME_MIN_LEN;
}

erps_msg_kind_t erps_msg_kind(const erps_raps_msg_t *m)
{
    switch (m->req) {
    case ERPS_RAPS_NR:
        return m->rb ? ERPS_MSG_NR_RB : ERPS_MSG_NR;
    case ERPS_RAPS_SF:
        return ERPS_MSG_SF;
    case ERPS_RAPS_MS:
        return ERPS_MSG_MS;
    case ERPS_RAPS_FS:
        return ERPS_MSG_FS;
    case ERPS_RAPS_EVENT:
        return ERPS_MSG_EVENT;
    default:
        return ERPS_MSG_COUNT;
    }
}

int erps_node_id_cmp(const uint8_t a[6], const uint8_t b[6])
{
    size_t i;

    for (i = 0; i < 6; ++i) {
        if (a[i] < b[i])
            return -1;
        if (a[i] > b[i])
            return 1;
    }
    return 0;
}
