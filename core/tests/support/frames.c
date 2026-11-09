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

/* Frames for the tests that need the wire form of a valid one, or of one
 * whose check fails. */
#include "frames.h"

#include <string.h>

#include "r2s/r2s_crc.h"
#include "r2s/r2s_framer.h"
#include "rcobs.h"

static uint8_t raw[R2S_MAX_FRAME + 1u];
static uint16_t raw_len;

static uint16_t finish(uint8_t *out)
{
    uint16_t len = r2s_test_rcobs_encode(raw, raw_len, out);

    out[len] = 0x00u;
    return (uint16_t)(len + 1u);
}

static uint16_t build(uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                      uint16_t payload_len)
{
    uint16_t crc;

    raw[0] = topic_id;
    raw[1] = seq;
    if (payload_len > 0u) {
        memcpy(raw + 2u, payload, payload_len);
    }
    raw_len = (uint16_t)(4u + payload_len);
    crc = r2s_crc16_update_buf(R2S_CRC16_INIT, raw, (uint16_t)(2u + payload_len));
    raw[2u + payload_len] = (uint8_t)(crc & 0xFFu);
    raw[3u + payload_len] = (uint8_t)(crc >> 8u);
    return finish(out);
}

uint16_t r2s_test_frame(uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                        uint16_t payload_len)
{
    return build(out, topic_id, seq, payload, payload_len);
}

uint16_t r2s_test_frame_bad_crc(uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                                uint16_t payload_len)
{
    (void)build(out, topic_id, seq, payload, payload_len);
    raw[raw_len - 1u] ^= 0x01u;
    return finish(out);
}
