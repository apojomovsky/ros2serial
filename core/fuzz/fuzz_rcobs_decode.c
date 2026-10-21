/* Fuzz target: the rCOBS decoder on arbitrary bytes. It must never crash,
 * read out of bounds or hang, whatever it is given. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "r2s/r2s_rcobs.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* The decoder works in place, so it needs a buffer it may write to. The
     * largest frame the protocol allows is 65277 bytes; anything longer is
     * cut, since the decoder would refuse it anyway. */
    static const size_t limit = 65535;
    uint8_t *buffer = malloc(limit);
    if (buffer == NULL) {
        return 0;
    }
    if (size > limit) {
        size = limit;
    }
    memcpy(buffer, data, size);

    uint8_t *decoded = NULL;
    int8_t status = r2s_rcobs_decode(buffer, (uint16_t)size, &decoded);
    if (status == R2S_RCOBS_OK) {
        /* The decoded frame must lie inside the buffer. */
        if (decoded < buffer || decoded > buffer + size) {
            abort();
        }
    }

    free(buffer);
    return 0;
}
