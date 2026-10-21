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

/* The largest length a varint can carry, and the number of bytes it takes
 * (VAR-2). */
#define R2S_VARINT_MAX 0xFFFFu
#define R2S_VARINT_MAX_BYTES 3u

/* Why a decoder refused an encoding. Each one names the rule of
 * spec/protocol.md, section 5, that the encoding breaks. */
#define R2S_VARINT_OK 0
#define R2S_VARINT_ERR_OVERFLOW (-1)     /* VAR-2 */
#define R2S_VARINT_ERR_TRUNCATED (-2)    /* VAR-3 */
#define R2S_VARINT_ERR_NOT_SHORTEST (-3) /* VAR-4 */

/* Writes value as a varint into out, which must hold R2S_VARINT_MAX_BYTES
 * bytes, and returns how many bytes it wrote. The encoding is the shortest
 * one (VAR-4), so 1 byte under 128, 2 under 16384 and 3 above. */
uint8_t r2s_varint_encode(uint16_t value, uint8_t *out);

/* Reads the varint at the start of data, which holds len bytes, and writes its
 * value and the number of bytes it used. Returns R2S_VARINT_OK, or the error
 * of the rule the encoding breaks: VAR-2 as soon as the third byte has its high
 * bit set, VAR-3 when the payload ends first, VAR-4 once the last byte is
 * read. On an error it writes neither value nor used. */
int8_t r2s_varint_decode(const uint8_t *data, uint16_t len, uint16_t *value, uint8_t *used);

#ifdef __cplusplus
}
#endif

#endif
