/* SPDX-License-Identifier: Apache-2.0 */

#include "erps/erps_pdu.h"
#include "harness.h"

#include <string.h>

static erps_raps_msg_t example_msg(void)
{
    erps_raps_msg_t msg = {0};
    const uint8_t node[6] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55};

    msg.req = ERPS_RAPS_SF;
    msg.dnf = true;
    msg.bpr = 1;
    memcpy(msg.node_id, node, sizeof(node));
    return msg;
}

static erps_frame_params_t example_params(void)
{
    erps_frame_params_t params = {0};
    const uint8_t src[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

    params.ring_id = 1;
    memcpy(params.src, src, sizeof(src));
    params.vid = 100;
    params.pcp = 7;
    params.mel = 7;
    params.version = 1;
    return params;
}

TEST(pdu_golden_frame)
{
    static const uint8_t expected[ERPS_FRAME_MIN_LEN] = {
        0x01, 0x19, 0xA7, 0x00, 0x00, 0x01,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
        0x81, 0x00, 0xE0, 0x64, 0x89, 0x02,
        0xE1, 0x28, 0x00, 0x20, 0xB0, 0x60,
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55
        /* The remaining 30 octets are Reserved2, End TLV, and padding. */
    };
    erps_frame_params_t params = example_params();
    erps_raps_msg_t msg = example_msg();
    uint8_t frame[ERPS_FRAME_MAX_LEN];
    size_t i;

    memset(frame, 0xA5, sizeof(frame));
    CHECK_EQ(erps_frame_encode(&params, &msg, frame, sizeof(frame)), ERPS_FRAME_MIN_LEN);
    CHECK_MEMEQ(frame, expected, sizeof(expected));
    for (i = ERPS_FRAME_MIN_LEN; i < sizeof(frame); ++i)
        CHECK_EQ(frame[i], 0xA5);
}

TEST(pdu_frame_round_trips)
{
    static const struct {
        uint8_t req;
        bool rb;
        bool dnf;
    } cases[] = {
        {ERPS_RAPS_NR, false, false},
        {ERPS_RAPS_NR, true, false},
        {ERPS_RAPS_SF, false, false},
        {ERPS_RAPS_SF, false, true},
        {ERPS_RAPS_MS, false, false},
        {ERPS_RAPS_FS, false, false},
        {ERPS_RAPS_EVENT, false, false}
    };
    erps_frame_params_t params = example_params();
    size_t c;
    unsigned bpr;
    unsigned mel;

    for (c = 0; c < sizeof(cases) / sizeof(cases[0]); ++c) {
        for (bpr = 0; bpr <= 1u; ++bpr) {
            for (mel = 0; mel <= 7u; ++mel) {
                erps_raps_msg_t msg = example_msg();
                erps_eth_t eth;
                erps_pdu_t decoded;
                uint8_t frame[ERPS_FRAME_MIN_LEN];
                erps_drop_t expected_drop;

                msg.req = cases[c].req;
                msg.rb = cases[c].rb;
                msg.dnf = cases[c].dnf;
                msg.bpr = (uint8_t)bpr;
                msg.status = 0xFF; /* The encoder must use the flag fields. */
                params.mel = (uint8_t)mel;

                CHECK_EQ(erps_frame_encode(&params, &msg, frame, sizeof(frame)), sizeof(frame));
                REQUIRE(erps_eth_parse(frame, sizeof(frame), &eth) == ERPS_DROP_NONE);
                CHECK(eth.tagged);
                CHECK_EQ(eth.vid, params.vid);
                CHECK_EQ(eth.pcp, params.pcp);
                CHECK_EQ(eth.ethertype, ERPS_ETHERTYPE_CFM);
                CHECK_EQ(eth.hdr_len, 18);
                CHECK_MEMEQ(eth.src, params.src, sizeof(eth.src));

                /* G.8032 §10.1.6 forbids a nonzero status on Event, including BPR=1. */
                expected_drop = msg.req == ERPS_RAPS_EVENT && bpr == 1u ?
                                ERPS_DROP_EVENT_SUBCODE : ERPS_DROP_NONE;
                CHECK_EQ(erps_pdu_decode(frame + eth.hdr_len,
                                         sizeof(frame) - eth.hdr_len, &decoded), expected_drop);
                CHECK_EQ(decoded.mel, mel);
                CHECK_EQ(decoded.version, params.version);
                CHECK_EQ(decoded.opcode, ERPS_OPCODE_RAPS);
                CHECK_EQ(decoded.tlv_offset, ERPS_TLV_OFFSET);
                CHECK_EQ(decoded.msg.req, msg.req);
                CHECK_EQ(decoded.msg.rb, msg.rb);
                CHECK_EQ(decoded.msg.dnf, msg.dnf);
                CHECK_EQ(decoded.msg.bpr, msg.bpr);
                CHECK_EQ(decoded.msg.status,
                         erps_status_octet(msg.rb, msg.dnf, msg.bpr));
                CHECK_MEMEQ(decoded.msg.node_id, msg.node_id, sizeof(msg.node_id));
            }
        }
    }
}

TEST(pdu_truncation_and_precedence)
{
    erps_raps_msg_t msg = example_msg();
    uint8_t pdu[ERPS_PDU_LEN];
    size_t len;

    REQUIRE(erps_pdu_encode(7, 1, &msg, pdu, sizeof(pdu)) == sizeof(pdu));
    for (len = 0; len < 36u; ++len) {
        erps_pdu_t out;

        memset(&out, 0xA5, sizeof(out));
        CHECK_EQ(erps_pdu_decode(pdu, len, &out), ERPS_DROP_SHORT);
        if (len < ERPS_PDU_HDR_LEN) {
            uint8_t sentinel[sizeof(out)];

            memset(sentinel, 0xA5, sizeof(sentinel));
            CHECK_MEMEQ(&out, sentinel, sizeof(out));
        }
    }

    {
        erps_pdu_t out;

        CHECK_EQ(erps_pdu_decode(pdu, 36, &out), ERPS_DROP_NONE);
        pdu[1] = 0;
        pdu[3] = 0;
        CHECK_EQ(erps_pdu_decode(pdu, 4, &out), ERPS_DROP_OPCODE);
        pdu[1] = ERPS_OPCODE_RAPS;
        pdu[3] = 31;
        CHECK_EQ(erps_pdu_decode(pdu, 4, &out), ERPS_DROP_TLV_OFFSET);
        CHECK_EQ(erps_pdu_decode(pdu, sizeof(pdu), &out), ERPS_DROP_TLV_OFFSET);
    }
}

TEST(pdu_reserved_requests_and_events)
{
    erps_raps_msg_t msg = example_msg();
    uint8_t pdu[ERPS_PDU_LEN];
    unsigned req;

    REQUIRE(erps_pdu_encode(7, 1, &msg, pdu, sizeof(pdu)) == sizeof(pdu));
    for (req = 0; req <= 15u; ++req) {
        erps_pdu_t out;

        if (req == ERPS_RAPS_NR || req == ERPS_RAPS_MS || req == ERPS_RAPS_SF ||
            req == ERPS_RAPS_FS || req == ERPS_RAPS_EVENT)
            continue;
        pdu[4] = (uint8_t)((req << 4) | 0x05u);
        CHECK_EQ(erps_pdu_decode(pdu, sizeof(pdu), &out), ERPS_DROP_REQ_RESERVED);
        CHECK_EQ(out.mel, 7);
        CHECK_EQ(out.version, 1);
        CHECK_EQ(out.opcode, ERPS_OPCODE_RAPS);
        CHECK_EQ(out.flags, 0);
        CHECK_EQ(out.tlv_offset, ERPS_TLV_OFFSET);
        CHECK_EQ(out.msg.req, req);
        CHECK_EQ(out.msg.subcode, 5);
        CHECK_EQ(out.msg.status, ERPS_STATUS_DNF | ERPS_STATUS_BPR);
        CHECK(!out.msg.rb);
        CHECK(out.msg.dnf);
        CHECK_EQ(out.msg.bpr, 1);
        CHECK_MEMEQ(out.msg.node_id, msg.node_id, sizeof(msg.node_id));
    }

    {
        erps_pdu_t out;

        pdu[4] = (uint8_t)((ERPS_RAPS_EVENT << 4) | 1u);
        pdu[5] = 0;
        CHECK_EQ(erps_pdu_decode(pdu, sizeof(pdu), &out), ERPS_DROP_EVENT_SUBCODE);
        CHECK_EQ(out.msg.req, ERPS_RAPS_EVENT);
        CHECK_EQ(out.msg.subcode, 1);
        CHECK_EQ(out.msg.status, 0);
        CHECK_MEMEQ(out.msg.node_id, msg.node_id, sizeof(msg.node_id));

        pdu[4] = (uint8_t)(ERPS_RAPS_EVENT << 4);
        pdu[5] = 1; /* A reserved status bit is still forbidden for Event. */
        CHECK_EQ(erps_pdu_decode(pdu, sizeof(pdu), &out), ERPS_DROP_EVENT_SUBCODE);
        CHECK_EQ(out.msg.subcode, 0);
        CHECK_EQ(out.msg.status, 1);
        CHECK_MEMEQ(out.msg.node_id, msg.node_id, sizeof(msg.node_id));
    }
}

TEST(pdu_lenient_receive_fields)
{
    erps_raps_msg_t msg = example_msg();
    erps_pdu_t out;
    uint8_t pdu[ERPS_PDU_LEN];

    REQUIRE(erps_pdu_encode(7, 1, &msg, pdu, sizeof(pdu)) == sizeof(pdu));
    pdu[0] = 0; /* Version 0 and MEL 0. */
    pdu[2] = 0xFF;
    pdu[3] = 33;
    pdu[4] = (uint8_t)((ERPS_RAPS_SF << 4) | 0x0Au);
    pdu[5] = 0x1F; /* Reserved status bits are ignored for a non-Event. */
    pdu[12] = 0xFF; /* Reserved2 is ignored. */
    pdu[36] = 0xFF; /* The End TLV is not checked. */
    CHECK_EQ(erps_pdu_decode(pdu, sizeof(pdu), &out), ERPS_DROP_NONE);
    CHECK_EQ(out.mel, 0);
    CHECK_EQ(out.version, 0);
    CHECK_EQ(out.flags, 0xFF);
    CHECK_EQ(out.tlv_offset, 33);
    CHECK_EQ(out.msg.subcode, 0x0A);
    CHECK_EQ(out.msg.status, 0x1F);
    CHECK(!out.msg.rb);
    CHECK(!out.msg.dnf);
    CHECK_EQ(out.msg.bpr, 0);

    pdu[0] = 0x1F; /* Version 31. */
    pdu[3] = 0xFF; /* An offset above 32 is accepted with only 36 octets. */
    CHECK_EQ(erps_pdu_decode(pdu, 36, &out), ERPS_DROP_NONE);
    CHECK_EQ(out.version, 31);
    CHECK_EQ(out.tlv_offset, 255);
}

TEST(pdu_ethernet_variants_and_truncation)
{
    const uint8_t untagged[14] = {
        0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60,
        0x12, 0x34
    };
    const uint8_t tagged[18] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        0x81, 0x00, 0xA1, 0x23, 0x89, 0x02
    };
    const uint8_t qinq[22] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
        0x88, 0xA8, 0xE1, 0x23, 0x81, 0x00,
        0x20, 0x45, 0x89, 0x02
    };
    erps_eth_t eth;
    size_t len;

    for (len = 0; len < sizeof(untagged); ++len)
        CHECK_EQ(erps_eth_parse(untagged, len, &eth), ERPS_DROP_SHORT);
    CHECK_EQ(erps_eth_parse(untagged, sizeof(untagged), &eth), ERPS_DROP_NONE);
    CHECK_MEMEQ(eth.dst, untagged, sizeof(eth.dst));
    CHECK_MEMEQ(eth.src, untagged + 6, sizeof(eth.src));
    CHECK(!eth.tagged);
    CHECK_EQ(eth.vid, 0);
    CHECK_EQ(eth.pcp, 0);
    CHECK_EQ(eth.ethertype, 0x1234);
    CHECK_EQ(eth.hdr_len, 14);

    for (len = 14; len < sizeof(tagged); ++len)
        CHECK_EQ(erps_eth_parse(tagged, len, &eth), ERPS_DROP_SHORT);
    CHECK_EQ(erps_eth_parse(tagged, sizeof(tagged), &eth), ERPS_DROP_NONE);
    CHECK(eth.tagged);
    CHECK_EQ(eth.vid, 0x123);
    CHECK_EQ(eth.pcp, 5);
    CHECK_EQ(eth.ethertype, ERPS_ETHERTYPE_CFM);
    CHECK_EQ(eth.hdr_len, 18);

    for (len = 18; len < sizeof(qinq); ++len)
        CHECK_EQ(erps_eth_parse(qinq, len, &eth), ERPS_DROP_SHORT);
    CHECK_EQ(erps_eth_parse(qinq, sizeof(qinq), &eth), ERPS_DROP_NONE);
    CHECK(eth.tagged);
    CHECK_EQ(eth.vid, 0x123); /* The inner VID is 0x045. */
    CHECK_EQ(eth.pcp, 7);
    CHECK_EQ(eth.ethertype, ERPS_ETHERTYPE_CFM);
    CHECK_EQ(eth.hdr_len, 22);

    {
        uint8_t third_tag[22];

        memcpy(third_tag, qinq, sizeof(third_tag));
        third_tag[20] = 0x81;
        third_tag[21] = 0x00;
        CHECK_EQ(erps_eth_parse(third_tag, sizeof(third_tag), &eth), ERPS_DROP_NONE);
        CHECK_EQ(eth.ethertype, ERPS_ETHERTYPE_VLAN);
        CHECK_EQ(eth.hdr_len, 22);
    }
}

TEST(pdu_dmac_status_kind_and_node_order)
{
    const uint8_t expected_prefix[5] = {0x01, 0x19, 0xA7, 0x00, 0x00};
    uint8_t dst[6];
    uint8_t ring_id;
    uint8_t a[6] = {0};
    uint8_t b[6] = {0};
    erps_raps_msg_t msg = {0};
    size_t i;

    CHECK_MEMEQ(erps_raps_dmac_prefix, expected_prefix, sizeof(expected_prefix));
    erps_raps_dmac(255, dst);
    CHECK_MEMEQ(dst, expected_prefix, sizeof(expected_prefix));
    CHECK_EQ(dst[5], 255);
    ring_id = 0;
    CHECK(erps_is_raps_dmac(dst, &ring_id));
    CHECK_EQ(ring_id, 255);
    CHECK(erps_is_raps_dmac(dst, NULL));
    for (i = 0; i < sizeof(expected_prefix); ++i) {
        dst[i] ^= 1u;
        ring_id = 42;
        CHECK(!erps_is_raps_dmac(dst, &ring_id));
        CHECK_EQ(ring_id, 42);
        dst[i] ^= 1u;
    }
    erps_raps_dmac(0, dst);
    CHECK(erps_is_raps_dmac(dst, &ring_id));
    CHECK_EQ(ring_id, 0);

    CHECK_EQ(erps_status_octet(false, false, 0), 0);
    CHECK_EQ(erps_status_octet(true, false, 0), 0x80);
    CHECK_EQ(erps_status_octet(false, true, 0), 0x40);
    CHECK_EQ(erps_status_octet(false, false, 1), 0x20);
    CHECK_EQ(erps_status_octet(true, true, 255), 0xE0);

    msg.req = ERPS_RAPS_NR;
    CHECK_EQ(erps_msg_kind(&msg), ERPS_MSG_NR);
    msg.rb = true;
    CHECK_EQ(erps_msg_kind(&msg), ERPS_MSG_NR_RB);
    msg.req = ERPS_RAPS_SF;
    CHECK_EQ(erps_msg_kind(&msg), ERPS_MSG_SF);
    msg.req = ERPS_RAPS_MS;
    CHECK_EQ(erps_msg_kind(&msg), ERPS_MSG_MS);
    msg.req = ERPS_RAPS_FS;
    CHECK_EQ(erps_msg_kind(&msg), ERPS_MSG_FS);
    msg.req = ERPS_RAPS_EVENT;
    CHECK_EQ(erps_msg_kind(&msg), ERPS_MSG_EVENT);
    msg.req = 0xF;
    CHECK_EQ(erps_msg_kind(&msg), ERPS_MSG_COUNT);

    CHECK_EQ(erps_node_id_cmp(a, b), 0);
    a[0] = 1;
    b[5] = 255;
    CHECK_EQ(erps_node_id_cmp(a, b), 1); /* First octet dominates the last. */
    CHECK_EQ(erps_node_id_cmp(b, a), -1);
    for (i = 0; i < sizeof(a); ++i) {
        memset(a, 0, sizeof(a));
        memset(b, 0, sizeof(b));
        b[i] = 1;
        CHECK_EQ(erps_node_id_cmp(a, b), -1);
        CHECK_EQ(erps_node_id_cmp(b, a), 1);
    }
}

TEST(pdu_encoder_bounds_and_canonical_fields)
{
    erps_frame_params_t params = example_params();
    erps_raps_msg_t msg = example_msg();
    uint8_t buf[ERPS_FRAME_MIN_LEN];
    size_t len;

    for (len = 0; len < ERPS_PDU_LEN; ++len) {
        memset(buf, 0xA5, sizeof(buf));
        CHECK_EQ(erps_pdu_encode(7, 1, &msg, buf, len), 0);
        CHECK_EQ(buf[0], 0xA5);
        CHECK_EQ(buf[sizeof(buf) - 1], 0xA5);
    }
    for (len = 0; len < ERPS_FRAME_MIN_LEN; ++len) {
        memset(buf, 0xA5, sizeof(buf));
        CHECK_EQ(erps_frame_encode(&params, &msg, buf, len), 0);
        CHECK_EQ(buf[0], 0xA5);
        CHECK_EQ(buf[sizeof(buf) - 1], 0xA5);
    }

    msg.req = ERPS_RAPS_MS;
    msg.subcode = 0xF;
    msg.status = 0xFF;
    REQUIRE(erps_pdu_encode(0xFF, 0xFF, &msg, buf, sizeof(buf)) == ERPS_PDU_LEN);
    CHECK_EQ(buf[0], 0xFF);
    CHECK_EQ(buf[1], ERPS_OPCODE_RAPS);
    CHECK_EQ(buf[2], 0);
    CHECK_EQ(buf[3], ERPS_TLV_OFFSET);
    CHECK_EQ(buf[4], ERPS_RAPS_MS << 4); /* Non-Event subcode is zeroed. */
    CHECK_EQ(buf[5], ERPS_STATUS_DNF | ERPS_STATUS_BPR);
    for (len = 12; len < ERPS_PDU_LEN; ++len)
        CHECK_EQ(buf[len], 0);

    msg.req = ERPS_RAPS_EVENT;
    msg.subcode = 0x1F;
    msg.rb = false;
    msg.dnf = false;
    msg.bpr = 0;
    REQUIRE(erps_pdu_encode(0, 0, &msg, buf, sizeof(buf)) == ERPS_PDU_LEN);
    CHECK_EQ(buf[4], 0xEF); /* Event preserves its low four subcode bits. */
}
