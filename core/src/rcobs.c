/* rcobs: the framing that removes every 0x00 from a frame. Stub until
 * implemented. */
#include "r2s/r2s_rcobs.h"

void r2s_rcobs_encoder_init(r2s_rcobs_encoder_t *enc)
{
    enc->run = 1u;
}

uint8_t r2s_rcobs_encode_byte(r2s_rcobs_encoder_t *enc, uint8_t byte, uint8_t *out)
{
    (void)enc;
    (void)byte;
    (void)out;
    return 0;
}

uint8_t r2s_rcobs_encode_end(r2s_rcobs_encoder_t *enc, uint8_t *out)
{
    (void)enc;
    (void)out;
    return 0;
}

int8_t r2s_rcobs_decode(uint8_t *buf, uint16_t len, uint8_t **decoded)
{
    (void)buf;
    (void)len;
    (void)decoded;
    return R2S_RCOBS_ERR_CODE;
}
