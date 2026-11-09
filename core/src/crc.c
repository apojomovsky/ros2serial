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

/* crc: CRC-16/MCRF4XX, bit by bit, so no 512-byte table costs flash on small
 * targets. Bytes in, the running check out (spec/protocol.md, section 3). */
#include "r2s/r2s_crc.h"

/* 0x1021 with its bits reversed, since input and output are reflected. */
#define POLY_REFLECTED 0x8408u

uint16_t r2s_crc16_update(uint16_t crc, uint8_t byte)
{
  crc ^= byte;
  for (uint8_t bit = 0; bit < 8u; bit++) {
    if (crc & 1u) {
      crc = (uint16_t)((crc >> 1) ^ POLY_REFLECTED);
    } else {
      crc = (uint16_t)(crc >> 1);
    }
  }
  return crc;
}

uint16_t r2s_crc16_update_buf(uint16_t crc, const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0; i < len; i++) {
    crc = r2s_crc16_update(crc, data[i]);
  }
  return crc;
}
