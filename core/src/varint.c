/* varint: lengths as unsigned LEB128, 7 bits per byte, least significant group
 * first (spec/protocol.md, section 5). */
#include "r2s/r2s_varint.h"

uint8_t r2s_varint_encode(uint16_t value, uint8_t *out)
{
    uint8_t written = 0;

    while (value >= 0x80u) {
        out[written++] = (uint8_t)(value | 0x80u);
        value = (uint16_t)(value >> 7);
    }
    out[written++] = (uint8_t)value;
    return written;
}

int8_t r2s_varint_decode(const uint8_t *data, uint16_t len, uint16_t *value, uint8_t *used)
{
    uint32_t result = 0;

    for (uint8_t i = 0; i < R2S_VARINT_MAX_BYTES; i++) {
        if (i >= len) {
            return R2S_VARINT_ERR_TRUNCATED;
        }
        result |= (uint32_t)(data[i] & 0x7Fu) << (7u * i);
        if ((data[i] & 0x80u) == 0u) {
            if (result > R2S_VARINT_MAX) {
                return R2S_VARINT_ERR_OVERFLOW;
            }
            /* A zero group above the first one means the value would fit in
             * fewer bytes, so the encoding is not the shortest (VAR-4). */
            if (i > 0u && data[i] == 0u) {
                return R2S_VARINT_ERR_NOT_SHORTEST;
            }
            *value = (uint16_t)result;
            *used = (uint8_t)(i + 1u);
            return R2S_VARINT_OK;
        }
    }
    return R2S_VARINT_ERR_OVERFLOW;
}
