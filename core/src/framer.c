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

/* The default instance behind the handle-free API: one node per firmware,
 * always on link 0. Hosts that frame several links own their instances. */
static r2s_framer_t default_framer;

static void drop(r2s_framer_t *framer)
{
    framer->counters.dropped_frames++;
}

void r2s_framer_count_refused(void)
{
    drop(&default_framer);
}

void r2s_framer_count_refused_inst(r2s_framer_t *framer)
{
    drop(framer);
}

void r2s_framer_init_inst(r2s_framer_t *framer, uint8_t link)
{
    framer->link = link;
    framer->len = 0u;
    framer->overflow = 0u;
    framer->ready = 0u;
    framer->ready_data = NULL;
    framer->ready_len = 0u;
    framer->ready_tick = 0u;
    framer->rep_overflow = 0u;
    framer->rep_toolarge = 0u;
    framer->rep_topic = 0xFFu;
    framer->counters.rx_frames = 0u;
    framer->counters.crc_errors = 0u;
    framer->counters.rx_overflows = 0u;
    framer->counters.dropped_frames = 0u;
}

void r2s_framer_init(void)
{
    r2s_framer_init_inst(&default_framer, 0u);
}

void r2s_framer_take_error_reports_inst(r2s_framer_t *framer, uint16_t *overflow, uint16_t *too_large,
                                        uint8_t *topic)
{
    *overflow = framer->rep_overflow;
    *too_large = framer->rep_toolarge;
    *topic = framer->rep_topic;
    framer->rep_overflow = 0u;
    framer->rep_toolarge = 0u;
    framer->rep_topic = 0xFFu;
}

void r2s_framer_take_error_reports(uint16_t *overflow, uint16_t *too_large, uint8_t *topic)
{
    r2s_framer_take_error_reports_inst(&default_framer, overflow, too_large, topic);
}

#ifdef R2S_TESTING
void r2s_framer_test_reset(void)
{
    r2s_framer_init();
}
#endif

uint8_t r2s_framer_poll_inst(r2s_framer_t *framer, r2s_frame_t *frame)
{
    if (framer->ready != 0u) {
        frame->data = framer->ready_data;
        frame->len = framer->ready_len;
        frame->tick = framer->ready_tick;
        return 1u;
    }
    for (;;) {
        uint8_t byte;
        uint32_t tick;
        uint8_t *decoded = NULL;
        uint16_t decoded_len = 0u;
        int8_t status;

        if (r2s_port_read_link(framer->link, &byte, 1u) != 1u) {
            return 0u;
        }
        if (byte != 0x00u) {
            if (framer->overflow != 0u) {
                continue;
            }
            if (framer->len >= R2S_FRAMER_RX_CAPACITY) {
                framer->overflow = 1u;
                continue;
            }
            framer->rx[framer->len] = byte;
            framer->len++;
            continue;
        }
        tick = r2s_port_tick_link(framer->link);
        if (framer->overflow != 0u) {
            framer->overflow = 0u;
            framer->len = 0u;
            framer->counters.rx_overflows++;
            /* The bytes never decode, so no topic is named ([ERR-4]). */
            framer->rep_overflow++;
            drop(framer);
            continue;
        }
        if (framer->len == 0u) {
            /* Two consecutive delimiters delimit an empty frame, ignored
             * without being counted (FRM-14). */
            continue;
        }
        status = r2s_rcobs_decode(framer->rx, framer->len, &decoded, &decoded_len);
        framer->len = 0u;
        if (status != R2S_RCOBS_OK) {
            drop(framer);
            continue;
        }
        if (decoded_len < FRAME_MIN || decoded_len > R2S_MAX_FRAME) {
            /* Short frames carry no topic byte; a long one decoded its
             * topic, so it names the frame its report carries. */
            if (decoded_len > R2S_MAX_FRAME) {
                framer->rep_toolarge++;
                framer->rep_topic = decoded[0];
            }
            drop(framer);
            continue;
        }
        if (r2s_crc16_update_buf(R2S_CRC16_INIT, decoded, decoded_len) != 0u) {
            framer->counters.crc_errors++;
            drop(framer);
            continue;
        }
        framer->ready = 1u;
        framer->ready_data = decoded;
        framer->ready_len = decoded_len;
        framer->ready_tick = tick;
        framer->counters.rx_frames++;
        frame->data = decoded;
        frame->len = decoded_len;
        frame->tick = tick;
        return 1u;
    }
}

uint8_t r2s_framer_poll(r2s_frame_t *frame)
{
    return r2s_framer_poll_inst(&default_framer, frame);
}

void r2s_framer_release_inst(r2s_framer_t *framer)
{
    framer->ready = 0u;
    framer->ready_data = NULL;
    framer->ready_len = 0u;
    framer->ready_tick = 0u;
}

void r2s_framer_release(void)
{
    r2s_framer_release_inst(&default_framer);
}

const r2s_framer_counters_t *r2s_framer_counters_inst(const r2s_framer_t *framer)
{
    return &framer->counters;
}

const r2s_framer_counters_t *r2s_framer_counters(void)
{
    return &default_framer.counters;
}
