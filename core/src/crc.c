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
