/* The rCOBS encoder, for the tests that need to build an encoding. */
#include "rcobs.h"

#include "r2s/r2s_rcobs.h"

uint16_t r2s_test_rcobs_encode(const uint8_t *data, uint16_t len, uint8_t *out)
{
    r2s_rcobs_encoder_t enc;
    uint16_t written = 0;

    r2s_rcobs_encoder_init(&enc);
    for (uint16_t i = 0; i < len; i++) {
        written = (uint16_t)(written + r2s_rcobs_encode_byte(&enc, data[i], out + written));
    }
    return (uint16_t)(written + r2s_rcobs_encode_end(&enc, out + written));
}
