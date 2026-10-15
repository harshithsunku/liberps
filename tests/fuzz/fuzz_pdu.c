/* SPDX-License-Identifier: Apache-2.0 */

#include "erps/erps_pdu.h"

#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static void check_pdu_round_trip(const uint8_t *data, size_t size)
{
    erps_pdu_t original;
    erps_pdu_t decoded;
    uint8_t encoded[ERPS_PDU_LEN];

    if (erps_pdu_decode(data, size, &original) != ERPS_DROP_NONE)
        return;
    if (erps_pdu_encode(original.mel, original.version, &original.msg,
                        encoded, sizeof(encoded)) != sizeof(encoded))
        abort();
    if (erps_pdu_decode(encoded, sizeof(encoded), &decoded) != ERPS_DROP_NONE)
        abort();
    if (decoded.msg.req != original.msg.req ||
        decoded.msg.rb != original.msg.rb ||
        decoded.msg.dnf != original.msg.dnf ||
        decoded.msg.bpr != original.msg.bpr ||
        memcmp(decoded.msg.node_id, original.msg.node_id,
               sizeof(original.msg.node_id)) != 0)
        abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    erps_eth_t eth;

    if (erps_eth_parse(data, size, &eth) == ERPS_DROP_NONE && eth.hdr_len <= size)
        check_pdu_round_trip(data + eth.hdr_len, size - eth.hdr_len);
    check_pdu_round_trip(data, size);
    return 0;
}
