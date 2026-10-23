/* Frames for the tests that need the wire form of a valid one, or of one
 * whose check fails. */
#include "frames.h"

#include <string.h>

#include "r2s/r2s_crc.h"
#include "r2s/r2s_framer.h"
#include "rcobs.h"

static uint8_t raw[R2S_MAX_FRAME + 1u];
static uint16_t raw_len;

static uint16_t finish(uint8_t *out)
{
    uint16_t len = r2s_test_rcobs_encode(raw, raw_len, out);

    out[len] = 0x00u;
    return (uint16_t)(len + 1u);
}

static uint16_t build(uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                      uint16_t payload_len)
{
    uint16_t crc;

    raw[0] = topic_id;
    raw[1] = seq;
    if (payload_len > 0u) {
        memcpy(raw + 2u, payload, payload_len);
    }
    raw_len = (uint16_t)(4u + payload_len);
    crc = r2s_crc16_update_buf(R2S_CRC16_INIT, raw, (uint16_t)(2u + payload_len));
    raw[2u + payload_len] = (uint8_t)(crc & 0xFFu);
    raw[3u + payload_len] = (uint8_t)(crc >> 8u);
    return finish(out);
}

uint16_t r2s_test_frame(uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                        uint16_t payload_len)
{
    return build(out, topic_id, seq, payload, payload_len);
}

uint16_t r2s_test_frame_bad_crc(uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                                uint16_t payload_len)
{
    (void)build(out, topic_id, seq, payload, payload_len);
    raw[raw_len - 1u] ^= 0x01u;
    return finish(out);
}
