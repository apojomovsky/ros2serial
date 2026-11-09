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

/* Length varints: unsigned LEB128, at most 3 bytes and 65535 (spec/protocol.md,
 * section 5). Not part of the firmware API: the codec uses it, and so does the
 * bridge, which links varint.c to read and write the lengths of a packed
 * payload as it converts to and from CDR. */
#ifndef R2S_VARINT_H
#define R2S_VARINT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The largest length a varint can carry (VAR-2).
#define R2S_VARINT_MAX 0xFFFFu
/// The bytes the largest length takes (VAR-2).
#define R2S_VARINT_MAX_BYTES 3u

// Why a decoder refused an encoding: the rule of spec/protocol.md, section 5,
// that the encoding breaks.
/// The encoding broke no rule.
#define R2S_VARINT_OK 0
/// The third byte still has its high bit set (VAR-2).
#define R2S_VARINT_ERR_OVERFLOW (-1)
/// The payload ended before the encoding did (VAR-3).
#define R2S_VARINT_ERR_TRUNCATED (-2)
/// Not the shortest encoding of its value (VAR-4).
#define R2S_VARINT_ERR_NOT_SHORTEST (-3)

/// Writes value as a varint into out.
/**
 * The encoding is the shortest one (VAR-4), so 1 byte under 128, 2 under
 * 16384 and 3 above.
 *
 * \param[in] value the length to write.
 * \param[out] out where to write the encoding.
 * \return How many bytes it wrote.
 * \pre out holds R2S_VARINT_MAX_BYTES bytes.
 */
uint8_t r2s_varint_encode(uint16_t value, uint8_t *out);

/// Reads the varint at the start of data.
/**
 * Writes its value and the number of bytes it used. On an error it writes
 * neither value nor used.
 *
 * \param[in] data the bytes to read.
 * \param[in] len how many bytes data holds.
 * \param[out] value the decoded length.
 * \param[out] used how many bytes the encoding took.
 * \return R2S_VARINT_OK, or the rule the encoding breaks: VAR-2 as soon as
 * the third byte has its high bit set, VAR-3 when the payload ends first,
 * VAR-4 once the last byte is read.
 */
int8_t r2s_varint_decode(const uint8_t *data, uint16_t len, uint16_t *value, uint8_t *used);

#ifdef __cplusplus
}
#endif

#endif
