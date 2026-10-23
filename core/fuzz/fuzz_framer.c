/* Fuzz target: the framer on arbitrary bytes pulled through the port. It must
 * never crash, read out of bounds or hang, an accepted frame must pass the CRC
 * check, and every delimiter must end at most one frame, accepted or dropped.
 * It builds with R2S_TESTING, like the core's host test builds. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "r2s/r2s_crc.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* The input as the port's RX stream, read front to back. */
static const uint8_t *feed;
static size_t feed_len;
static size_t feed_pos;

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    size_t left = feed_len - feed_pos;
    size_t n = left < max ? left : max;

    if (n > 0u) {
        memcpy(buf, feed + feed_pos, n);
        feed_pos += n;
    }
    return (uint16_t)n;
}

uint32_t r2s_port_tick(void)
{
    return 0u;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    size_t delimiters = 0;
    size_t counted;

    feed = data;
    feed_len = size;
    feed_pos = 0u;
    for (size_t i = 0; i < size; i++) {
        delimiters += data[i] == 0x00u;
    }

    r2s_framer_init();
    for (;;) {
        r2s_frame_t frame;

        if (r2s_framer_poll(&frame) == 0u) {
            break;
        }
        if (frame.data == NULL || frame.len < 4u || frame.len > R2S_MAX_FRAME) {
            abort();
        }
        if (r2s_crc16_update_buf(R2S_CRC16_INIT, frame.data, frame.len) != 0u) {
            abort();
        }
        r2s_framer_release();
    }
    if (feed_pos != feed_len) {
        abort();
    }
    counted = r2s_framer_counters()->rx_frames + (size_t)r2s_framer_counters()->dropped_frames;
    if (counted > delimiters) {
        abort();
    }
    return 0;
}
