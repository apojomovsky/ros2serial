/* varint: lengths as unsigned LEB128. Stub until implemented. */
#include "r2s/r2s_varint.h"

uint8_t r2s_varint_encode(uint16_t value, uint8_t *out)
{
    (void)value;
    (void)out;
    return 0;
}

int8_t r2s_varint_decode(const uint8_t *data, uint16_t len, uint16_t *value, uint8_t *used)
{
    (void)data;
    (void)len;
    (void)value;
    (void)used;
    return R2S_VARINT_ERR_TRUNCATED;
}
