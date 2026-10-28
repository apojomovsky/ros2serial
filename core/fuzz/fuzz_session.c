/* Fuzz target: the MCU session on arbitrary bytes from the host. The input
 * is the RX stream; the tick advances with every port call so heartbeats,
 * retransmits and the link timeout fire without an endless loop. The session
 * must take any input without crashing, hanging or touching memory out of
 * bounds; the sanitizers are the oracle. User-topic frames decode through
 * the fuzz table, so the run reaches the codec through the session too. The
 * harness only drives the session API and seeds from spec vectors, with no
 * HELLO byte layout of its own, so the S8 token change rebases cleanly. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"

#include "fuzz_codec_table.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* The input as the host's byte stream, read front to back. */
static const uint8_t *feed;
static size_t feed_len;
static size_t feed_pos;
static uint32_t tick;

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    size_t left = feed_len - feed_pos;
    size_t n = left < max ? left : max;

    tick += 64u;
    if (n > 0u) {
        memcpy(buf, feed + feed_pos, n);
        feed_pos += n;
    }
    return (uint16_t)n;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
}

uint16_t r2s_port_tx_free(void)
{
    return 0xFFFFu;
}

uint32_t r2s_port_tick(void)
{
    tick += 64u;
    return tick;
}

uint32_t r2s_port_boot_id(void)
{
    return 0x12345678u;
}

void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    static fuzz_msg_union_t msg;
    uint16_t type = (uint16_t)((topic_id + seq) % FUZZ_CODEC_TYPE_COUNT);
    uint8_t field = 0u;

    (void)r2s_codec_decode(&FUZZ_CODEC_TABLE, type, payload, len, (uint8_t)(seq & 1u), &msg, &field);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const r2s_topic_t topics[] = {
        {0x10u,
         0x00u,
         {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u},
         "std_msgs/msg/Float32",
         "battery"},
        {0x11u,
         0x01u,
         {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u},
         "geometry_msgs/msg/Twist",
         "cmd_vel"},
    };
    uint32_t rounds;

    feed = data;
    feed_len = size;
    feed_pos = 0u;
    tick = 0u;
    r2s_framer_init();
    if (r2s_session_configure("node", topics, 2u) != R2S_OK) {
        abort();
    }
    r2s_init();
    for (rounds = 0u; rounds < 16u && (feed_pos < feed_len || rounds < 8u); rounds++) {
        r2s_spin_some();
    }
    return 0;
}
