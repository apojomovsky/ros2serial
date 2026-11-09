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
#ifndef R2S_TEST_FRAMES_H
#define R2S_TEST_FRAMES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes one frame, delimiter included, into out and returns its length:
 * topic_id, seq, the payload, its CRC-16/MCRF4XX and the rCOBS encoding
 * (FRM-1, FRM-2, CHK-3). out must hold the COB-8 bound of 4 + payload_len
 * bytes, plus the delimiter. */
uint16_t r2s_test_frame(
  uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
  uint16_t payload_len);

/* The same frame with its second CRC byte flipped, so the receiver's check
 * fails (FRM-8). */
uint16_t r2s_test_frame_bad_crc(
  uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
  uint16_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
