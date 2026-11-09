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

/* Fuzz target: the codec interpreter on arbitrary payloads. The first byte
 * picks the record, the second the narrow flag, the rest is the payload. The
 * decoder must accept or refuse every input without crashing, hanging or
 * touching memory past the message; the sanitizers are the oracle. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "r2s/r2s_codec.h"
#include "r2s/r2s_port.h"

#include "fuzz_codec_table.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* The encoder the interpreter shares its table with writes through the
 * port, which the decode-only run discards. */
void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  (void)buf;
  (void)len;
}

uint16_t r2s_port_tx_free(void)
{
  return 0xFFFFu;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  static fuzz_msg_union_t msg;
  uint16_t type;
  uint8_t narrow;
  uint8_t field = 0u;
  int8_t status;

  if (size < 2u) {
    return 0;
  }
  type = (uint16_t)(data[0] % FUZZ_CODEC_TYPE_COUNT);
  narrow = (uint8_t)(data[1] & 1u);
  memset(&msg, 0, sizeof msg);
  status =
    r2s_codec_decode(&FUZZ_CODEC_TABLE, type, data + 2u, (uint16_t)(size - 2u), narrow, &msg,
    &field);
  if (status != R2S_OK && status != R2S_ERR_BOUNDS && status != R2S_ERR_DECODE) {
    abort();
  }
  return 0;
}
