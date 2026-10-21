/* rcobs: the framing that removes every 0x00 from a frame, so a 0x00 can
 * delimit it (spec/protocol.md, section 4). */
#include "r2s/r2s_rcobs.h"

void r2s_rcobs_encoder_init(r2s_rcobs_encoder_t *enc)
{
    enc->run = 1u;
}

uint8_t r2s_rcobs_encode_byte(r2s_rcobs_encoder_t *enc, uint8_t byte, uint8_t *out)
{
    if (byte == 0x00u) {
        out[0] = enc->run;
        enc->run = 1u;
        return 1u;
    }
    out[0] = byte;
    enc->run = (uint8_t)(enc->run + 1u);
    if (enc->run == 0xFFu) {
        /* A group of 254 bytes ends with its own code byte, and the next
         * group starts empty. */
        out[1] = 0xFFu;
        enc->run = 1u;
        return 2u;
    }
    return 1u;
}

uint8_t r2s_rcobs_encode_end(r2s_rcobs_encoder_t *enc, uint8_t *out)
{
    out[0] = enc->run;
    enc->run = 1u;
    return 1u;
}

int8_t r2s_rcobs_decode(uint8_t *buf, uint16_t len, uint8_t **decoded, uint16_t *decoded_len)
{
    uint16_t read = len;
    uint16_t write = len;
    uint8_t seen = 0;

    if (len == 0u) {
        *decoded = buf;
        *decoded_len = 0u;
        return R2S_RCOBS_OK;
    }
    /* COB-6 comes before any code is read: an encoding that ends in 0xFF has
     * a second reading of the same bytes. */
    if (buf[len - 1u] == 0xFFu) {
        return R2S_RCOBS_ERR_TRAILING;
    }
    /* COB-5 refuses an encoding that holds a 0x00 byte anywhere, since the
     * encoding never holds one. */
    for (uint16_t i = 0; i < len; i++) {
        if (buf[i] == 0x00u) {
            return R2S_RCOBS_ERR_CODE;
        }
    }

    while (read > 0u) {
        uint8_t code = buf[read - 1u];

        if ((uint16_t)code > read) {
            return R2S_RCOBS_ERR_CODE;
        }
        if (seen != 0u && code != 0xFFu) {
            /* A group that is not the one nearest the end is followed by the
             * 0x00 it stands for. */
            buf[--write] = 0x00u;
        }
        for (uint8_t k = 1u; k < code; k++) {
            buf[--write] = buf[read - 1u - k];
        }
        read = (uint16_t)(read - code);
        seen = 1u;
    }

    *decoded = buf + write;
    *decoded_len = (uint16_t)(len - write);
    return R2S_RCOBS_OK;
}
