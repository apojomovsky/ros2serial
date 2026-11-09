// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

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
