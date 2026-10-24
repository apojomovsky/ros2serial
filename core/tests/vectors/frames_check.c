/* Conformance: every frame vector through the transmit path, which must put
 * the vector's wire bytes on the port. */
#include <stdio.h>

#include "bytes.h"
#include "fake_port.h"
#include "r2s/r2s_tx.h"
#include "unity.h"
#include "vectors.h"

void r2s_vec_check_frames(const r2s_vec_frames_t *v)
{
    char label[256];
    uint16_t len;
    const uint8_t *tx;

    snprintf(label, sizeof label, "%s [%s]", v->where, v->covers);
    TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, r2s_tx_begin(v->topic_id, v->seq, v->payload_len), label);
    for (uint16_t i = 0; i < v->payload_len; i++) {
        TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, r2s_tx_byte(v->payload[i]), label);
    }
    TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, r2s_tx_end(), label);
    tx = fake_port_tx(&len);
    R2S_ASSERT_BYTES(v->wire, v->wire_len, tx, len, label);
}
