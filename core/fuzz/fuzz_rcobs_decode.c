/* Fuzz target: the rCOBS decoder on arbitrary bytes. It must never crash,
 * read out of bounds or hang, whatever it is given, and whatever it accepts
 * must be the canonical encoding of the bytes it decoded. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "r2s/r2s_rcobs.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* The decoder works in place, so it needs a buffer it may write to, and
     * a copy of the input to compare against afterwards. The largest frame
     * the protocol allows is 65277 bytes; anything longer is cut, since the
     * decoder would refuse it anyway. */
    static uint8_t buffer[65535];
    static uint8_t original[65535];
    static uint8_t reencoded[65535];
    uint8_t *decoded = NULL;
    uint16_t decoded_len = 0;

    if (size > sizeof buffer) {
        size = sizeof buffer;
    }
    memcpy(buffer, data, size);
    memcpy(original, data, size);

    int8_t status = r2s_rcobs_decode(buffer, (uint16_t)size, &decoded, &decoded_len);
    if (status != R2S_RCOBS_OK) {
        return 0;
    }
    /* The decoded frame must lie inside the buffer, and its length must fit
     * in it. */
    if (decoded < buffer || decoded > buffer + size) {
        abort();
    }
    if (decoded_len > size || decoded != buffer + (size - decoded_len)) {
        abort();
    }

    /* Whatever the decoder accepted must be the encoding of the bytes it
     * decoded, or a frame would have a second reading. The empty input is
     * the one exception: it decodes to an empty frame, whose encoding is
     * one byte. */
    if (size == 0) {
        return 0;
    }
    r2s_rcobs_encoder_t enc;
    uint16_t written = 0;
    r2s_rcobs_encoder_init(&enc);
    for (uint16_t i = 0; i < decoded_len; i++) {
        written = (uint16_t)(written + r2s_rcobs_encode_byte(&enc, decoded[i], reencoded + written));
    }
    written = (uint16_t)(written + r2s_rcobs_encode_end(&enc, reencoded + written));
    if (written != size || memcmp(reencoded, original, size) != 0) {
        abort();
    }
    return 0;
}
