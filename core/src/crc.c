/* crc: CRC-16/MCRF4XX, bit by bit. Stub until implemented. */
#include "r2s/r2s_crc.h"

uint16_t r2s_crc16_update(uint16_t crc, uint8_t byte)
{
    (void)byte;
    return crc;
}

uint16_t r2s_crc16_update_buf(uint16_t crc, const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
    return crc;
}
