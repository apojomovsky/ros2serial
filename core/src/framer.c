/* framer: bytes in through r2s_port_read, decoded frames out. It cuts frames
 * on 0x00, decodes rCOBS in place and checks the CRC (spec/protocol.md,
 * sections 2 to 4). */
#include "r2s/r2s_framer.h"

#include <stddef.h>

#include "r2s/r2s_crc.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_rcobs.h"

/* The shortest decoded frame: topic_id, seq and crc16 (FRM-7). */
#define FRAME_MIN 4u

static struct {
    uint8_t rx[R2S_FRAMER_RX_CAPACITY];
    uint16_t len;     /* encoded bytes of the frame in progress */
    uint8_t overflow; /* over the buffer: discard bytes until the next 0x00 */
    uint8_t ready;    /* a decoded frame waits for r2s_framer_release() */
    const uint8_t *ready_data;
    uint16_t ready_len;
    uint32_t ready_tick;
    uint16_t rep_overflow; /* receive overflows since the last take */
    uint16_t rep_toolarge; /* frames past the limit since the last take */
    uint8_t rep_topic;     /* the topic of the last frame past the limit */
    r2s_framer_counters_t counters;
} framer;

static void drop(void)
{
    framer.counters.dropped_frames++;
}

void r2s_framer_count_refused(void)
{
    drop();
}

void r2s_framer_init(void)
{
    framer.len = 0u;
    framer.overflow = 0u;
    framer.ready = 0u;
    framer.ready_data = NULL;
    framer.ready_len = 0u;
    framer.ready_tick = 0u;
    framer.rep_overflow = 0u;
    framer.rep_toolarge = 0u;
    framer.rep_topic = 0xFFu;
    framer.counters.rx_frames = 0u;
    framer.counters.crc_errors = 0u;
    framer.counters.rx_overflows = 0u;
    framer.counters.dropped_frames = 0u;
}

void r2s_framer_take_error_reports(uint16_t *overflow, uint16_t *too_large, uint8_t *topic)
{
    *overflow = framer.rep_overflow;
    *too_large = framer.rep_toolarge;
    *topic = framer.rep_topic;
    framer.rep_overflow = 0u;
    framer.rep_toolarge = 0u;
    framer.rep_topic = 0xFFu;
}

#ifdef R2S_TESTING
void r2s_framer_test_reset(void)
{
    r2s_framer_init();
}
#endif

uint8_t r2s_framer_poll(r2s_frame_t *frame)
{
    if (framer.ready != 0u) {
        frame->data = framer.ready_data;
        frame->len = framer.ready_len;
        frame->tick = framer.ready_tick;
        return 1u;
    }
    for (;;) {
        uint8_t byte;
        uint32_t tick;
        uint8_t *decoded = NULL;
        uint16_t decoded_len = 0u;
        int8_t status;

        if (r2s_port_read(&byte, 1u) != 1u) {
            return 0u;
        }
        if (byte != 0x00u) {
            if (framer.overflow != 0u) {
                continue;
            }
            if (framer.len >= R2S_FRAMER_RX_CAPACITY) {
                framer.overflow = 1u;
                continue;
            }
            framer.rx[framer.len] = byte;
            framer.len++;
            continue;
        }
        tick = r2s_port_tick();
        if (framer.overflow != 0u) {
            framer.overflow = 0u;
            framer.len = 0u;
            framer.counters.rx_overflows++;
            /* The bytes never decode, so no topic is named ([ERR-4]). */
            framer.rep_overflow++;
            drop();
            continue;
        }
        if (framer.len == 0u) {
            /* Two consecutive delimiters delimit an empty frame, ignored
             * without being counted (FRM-14). */
            continue;
        }
        status = r2s_rcobs_decode(framer.rx, framer.len, &decoded, &decoded_len);
        framer.len = 0u;
        if (status != R2S_RCOBS_OK) {
            drop();
            continue;
        }
        if (decoded_len < FRAME_MIN || decoded_len > R2S_MAX_FRAME) {
            /* Short frames carry no topic byte; a long one decoded its
             * topic, so it names the frame its report carries. */
            if (decoded_len > R2S_MAX_FRAME) {
                framer.rep_toolarge++;
                framer.rep_topic = decoded[0];
            }
            drop();
            continue;
        }
        if (r2s_crc16_update_buf(R2S_CRC16_INIT, decoded, decoded_len) != 0u) {
            framer.counters.crc_errors++;
            drop();
            continue;
        }
        framer.ready = 1u;
        framer.ready_data = decoded;
        framer.ready_len = decoded_len;
        framer.ready_tick = tick;
        framer.counters.rx_frames++;
        frame->data = decoded;
        frame->len = decoded_len;
        frame->tick = tick;
        return 1u;
    }
}

void r2s_framer_release(void)
{
    framer.ready = 0u;
    framer.ready_data = NULL;
    framer.ready_len = 0u;
    framer.ready_tick = 0u;
}

const r2s_framer_counters_t *r2s_framer_counters(void)
{
    return &framer.counters;
}
