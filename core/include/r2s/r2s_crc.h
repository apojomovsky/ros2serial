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

/* The frame check, CRC-16/MCRF4XX (spec/protocol.md, section 3). Not part of
 * the firmware API: the core's framing uses it, and so does the bridge, which
 * links crc.c for its own framing. */
#ifndef R2S_CRC_H
#define R2S_CRC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The value a CRC starts from before the first byte (CHK-1). */
#define R2S_CRC16_INIT 0xFFFFu

/* Returns the CRC after feeding one more byte to crc. Feeding a frame's bytes
 * one at a time from R2S_CRC16_INIT gives its frame check; feeding the check
 * too, little-endian, gives 0 (CHK-4). */
uint16_t r2s_crc16_update(uint16_t crc, uint8_t byte);

/* Returns the CRC after feeding len bytes of data to crc, the same as calling
 * r2s_crc16_update() once per byte. data may be NULL when len is 0. */
uint16_t r2s_crc16_update_buf(uint16_t crc, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
