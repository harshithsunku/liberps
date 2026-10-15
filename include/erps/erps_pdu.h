/* SPDX-License-Identifier: Apache-2.0 */
/*
 * R-APS PDU codec (G.8032 §10.3, G.8013 common OAM header). Pure functions, no state.
 * Used by the core and available to platform adapters and tools (demux, capture decoding).
 *
 * Wire layout of a tagged R-APS frame (offsets in octets):
 *   0  DA   01-19-A7-00-00-<ring id>
 *   6  SA
 *   12 TPID 0x8100 | 14 TCI (PCP:3 DEI:1 VID:12)
 *   16 EtherType 0x8902
 *   18 PDU: MEL(3)|Version(5), OpCode=40, Flags=0, TLV Offset=32,
 *           R-APS info (32 octets: Req/State(4)|Sub-code(4), Status, Node ID(6), Reserved2(24)),
 *           End TLV (0)
 *   55 padding to 60 octets (minimum frame without FCS)
 */
#ifndef ERPS_PDU_H
#define ERPS_PDU_H

#include "erps.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ERPS_ETHERTYPE_CFM   0x8902u
#define ERPS_ETHERTYPE_VLAN  0x8100u
#define ERPS_ETHERTYPE_QINQ  0x88A8u
#define ERPS_OPCODE_RAPS     40u
#define ERPS_TLV_OFFSET      32u
#define ERPS_RAPS_INFO_LEN   32u
#define ERPS_PDU_HDR_LEN     4u
#define ERPS_PDU_LEN         (ERPS_PDU_HDR_LEN + ERPS_RAPS_INFO_LEN + 1u) /* 37, incl. End TLV */
#define ERPS_ETH_HDR_LEN     14u
#define ERPS_VLAN_TAG_LEN    4u
#define ERPS_FRAME_MIN_LEN   60u  /* minimum Ethernet frame without FCS */
#define ERPS_FRAME_MAX_LEN   64u  /* buffer size sufficient for erps_frame_encode() */

#define ERPS_STATUS_RB       0x80u /* bit 8 */
#define ERPS_STATUS_DNF      0x40u /* bit 7 */
#define ERPS_STATUS_BPR      0x20u /* bit 6 */

#define ERPS_VERSION_V1      0x00u /* G.8032 (2008) */
#define ERPS_VERSION_V2      0x01u /* this edition */

/** Fixed DA prefix 01-19-A7-00-00 (ITU OUI range for R-APS, §10.3). */
ERPS_API extern const uint8_t erps_raps_dmac_prefix[5];

/** Parsed Ethernet header. */
typedef struct {
    uint8_t dst[6];
    uint8_t src[6];
    bool tagged;          /**< an 802.1Q (or 802.1ad) tag was present */
    uint16_t vid;         /**< VID of the outermost tag (0 if untagged) */
    uint8_t pcp;
    uint16_t ethertype;   /**< EtherType after all tags */
    size_t hdr_len;       /**< offset of the payload (PDU) */
} erps_eth_t;

/** Decoded R-APS PDU: common OAM header + R-APS specific information. */
typedef struct {
    uint8_t mel;          /**< 0..7 */
    uint8_t version;      /**< 0..31 */
    uint8_t opcode;
    uint8_t flags;
    uint8_t tlv_offset;
    erps_raps_msg_t msg;
} erps_pdu_t;

/** Parameters for building a frame. */
typedef struct {
    uint8_t ring_id;      /**< DA last octet */
    uint8_t src[6];
    uint16_t vid;         /**< 1..4094 */
    uint8_t pcp;          /**< 0..7 */
    uint8_t mel;          /**< 0..7 */
    uint8_t version;      /**< 0..31 */
} erps_frame_params_t;

/** Build the R-APS DA 01-19-A7-00-00-<ring_id>. */
ERPS_API void erps_raps_dmac(uint8_t ring_id, uint8_t out[6]);

/** True if @p dst is in 01-19-A7-00-00-xx; stores xx in @p ring_id (may be NULL). */
ERPS_API bool erps_is_raps_dmac(const uint8_t dst[6], uint8_t *ring_id);

/**
 * Parse the Ethernet header (up to two VLAN tags). Returns ERPS_DROP_NONE on success or
 * ERPS_DROP_SHORT if the frame is truncated. Does not check DA / EtherType values.
 */
ERPS_API erps_drop_t erps_eth_parse(const uint8_t *frame, size_t len, erps_eth_t *eth);

/**
 * Decode an R-APS PDU (starting at MEL/Version). Checks length, OpCode (40), TLV offset (>= 32)
 * and the Request/State and Event sub-code validity rules of §10.1.6. Does not check MEL or
 * ring ID (instance-specific). On ERPS_DROP_REQ_RESERVED / _EVENT_SUBCODE @p out is filled.
 */
ERPS_API erps_drop_t erps_pdu_decode(const uint8_t *pdu, size_t len, erps_pdu_t *out);

/**
 * Encode an R-APS PDU (37 octets incl. End TLV). Reserved fields are zero, TLV offset = 32,
 * OpCode = 40. The status octet is built from msg.rb/dnf/bpr (msg.status is ignored).
 * Returns the number of octets written, or 0 if @p len < ERPS_PDU_LEN.
 */
ERPS_API size_t erps_pdu_encode(uint8_t mel, uint8_t version, const erps_raps_msg_t *msg,
                                uint8_t *buf, size_t len);

/**
 * Encode a complete tagged R-APS frame, padded to ERPS_FRAME_MIN_LEN. Returns its length, or
 * 0 if @p len < ERPS_FRAME_MIN_LEN.
 */
ERPS_API size_t erps_frame_encode(const erps_frame_params_t *p, const erps_raps_msg_t *msg,
                                  uint8_t *buf, size_t len);

/** Build a status octet from flags. */
ERPS_API uint8_t erps_status_octet(bool rb, bool dnf, uint8_t bpr);

/** Message kind for counters (NR vs NR,RB etc.); ERPS_MSG_COUNT if reserved. */
ERPS_API erps_msg_kind_t erps_msg_kind(const erps_raps_msg_t *m);

/** Compare node IDs as 48-bit unsigned numbers (first octet most significant): <0, 0, >0. */
ERPS_API int erps_node_id_cmp(const uint8_t a[6], const uint8_t b[6]);

#ifdef __cplusplus
}
#endif

#endif /* ERPS_PDU_H */
