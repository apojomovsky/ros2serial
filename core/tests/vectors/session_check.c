/* Conformance: every handshake transcript through the MCU session. The host
 * steps go onto the fake port byte by byte at their own times, and every MCU
 * step must come out of the session in order, with the step and the frame
 * named when one does not. */
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "unity.h"
#include "vectors.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

/* The announced table of the transcript firmware: the shape of the TOPIC
 * steps, so the TOPIC the core sends carries the vector's payload. */
static const r2s_topic_t transcript_topics[] = {
    {0x10u,
     0x00u,
     {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u},
     "std_msgs/msg/Float32",
     "battery"},
    {0x11u,
     0x05u,
     {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u},
     "geometry_msgs/msg/Twist",
     "cmd_vel"},
};

/* The pub_sub firmware announces the same topics with narrow hashes. */
static const r2s_topic_t pub_sub_topics[] = {
    {0x10u,
     0x00u,
     {0x71u, 0x70u, 0xd3u, 0xd8u, 0xf8u, 0x41u, 0xf7u, 0xbeu},
     "std_msgs/msg/Float32",
     "battery"},
    {0x11u,
     0x03u,
     {0x9cu, 0x45u, 0xbfu, 0x16u, 0xfeu, 0x09u, 0x83u, 0xd8u},
     "geometry_msgs/msg/Twist",
     "cmd_vel"},
};

static void configure(const r2s_vec_session_t *v)
{
    if (strcmp(v->name, "empty_topics") == 0) {
        TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, r2s_session_configure("node", NULL, 0u), v->where);
    } else if (strcmp(v->name, "pub_sub_handshake") == 0) {
        TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, r2s_session_configure("demo", pub_sub_topics, 2u), v->where);
    } else {
        TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, r2s_session_configure("node", transcript_topics, 2u),
                                       v->where);
    }
    r2s_init();
}

/* Moves the tick to at_ms, spinning every millisecond, so the timers fire as
 * the transcript advances. */
static void advance_to(uint32_t *now, uint32_t at_ms)
{
    while (*now < at_ms) {
        fake_port_advance(1u);
        (*now)++;
        r2s_spin_some();
    }
}

/* Queues one decoded frame's wire bytes one at a time, spinning after each,
 * as a slow link delivers them. */
static void feed_wire(const uint8_t *decoded, uint16_t decoded_len, const char *where)
{
    static uint8_t wire[WIRE_MAX];
    char label[256];
    uint16_t wire_len;

    snprintf(label, sizeof label, "%s: host frame", where);
    TEST_ASSERT_TRUE_MESSAGE(decoded_len >= 4u, label);
    wire_len = r2s_test_frame(wire, decoded[0], decoded[1], decoded + 2u, (uint16_t)(decoded_len - 4u));
    for (uint16_t i = 0u; i < wire_len; i++) {
        fake_port_queue_rx(wire + i, 1u);
        r2s_spin_some();
    }
}

/* The wire form of one decoded MCU step, with the transcript firmware's tick
 * rate and features replaced by this build's own: the vectors pin a 1 MHz
 * firmware without logging, and the host test build announces its own tick
 * and features (CTL-7, CTL-8). The substitution is evidence tooling only. */
static uint16_t expected_wire(uint8_t *out, const uint8_t *decoded, uint16_t decoded_len)
{
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t payload_len = (uint16_t)(decoded_len - 4u);

    memcpy(payload, decoded + 2u, payload_len);
    if (payload_len >= 16u && payload[0] == 0x02u) {
        payload[11] = (uint8_t)(R2S_TICK_HZ & 0xFFu);
        payload[12] = (uint8_t)((R2S_TICK_HZ >> 8u) & 0xFFu);
        payload[13] = (uint8_t)((R2S_TICK_HZ >> 16u) & 0xFFu);
        payload[14] = (uint8_t)((R2S_TICK_HZ >> 24u) & 0xFFu);
#if DBL_MANT_DIG == FLT_MANT_DIG
        payload[15] = 0x12u;
#else
        payload[15] = 0x02u;
#endif
    }
    return r2s_test_frame(out, decoded[0], decoded[1], payload, payload_len);
}

void r2s_vec_check_session(const r2s_vec_session_t *v)
{
    static uint8_t want[WIRE_MAX];
    char label[256];
    char hex[R2S_HEX_TEXT_MAX];
    uint32_t now = 0u;
    uint16_t produced = 0u;
    uint16_t matched = 0u;
    uint16_t mcu_steps = 0u;

    /* The MCU boots when its first frame goes out: host traffic before that
     * went nowhere, since the port was not open yet (SES-12, SES-13). */
    for (uint16_t i = 0u; i < v->nsteps; i++) {
        if (!v->steps[i].from_host) {
            now = v->steps[i].at_ms;
            break;
        }
    }
    fake_port_set_tick(now);
    configure(v);
    for (uint16_t i = 0u; i < v->nsteps; i++) {
        if (v->steps[i].at_ms < now) {
            continue;
        }
        advance_to(&now, v->steps[i].at_ms);
        if (v->steps[i].from_host) {
            feed_wire(v->steps[i].frame, v->steps[i].frame_len, v->where);
        }
    }
    for (uint16_t i = 0u; i < v->nsteps; i++) {
        const uint8_t *frame;
        uint16_t frame_len;
        uint16_t want_len;
        uint8_t found = 0u;

        if (v->steps[i].from_host) {
            continue;
        }
        mcu_steps++;
        want_len = expected_wire(want, v->steps[i].frame, v->steps[i].frame_len);
        want_len--;
        for (; matched < fake_port_tx_frame_count(); matched++) {
            frame = fake_port_tx_frame(matched, &frame_len);
            if (frame_len == want_len && memcmp(frame, want, want_len) == 0) {
                found = 1u;
                matched++;
                break;
            }
            produced++;
        }
        if (!found) {
            uint16_t total = fake_port_tx_frame_count();

            r2s_hex_format(want, want_len > 64u ? 64u : want_len, hex, sizeof hex);
            printf("%s [%s] step %u at %ums: want MCU frame %s\n", v->where, v->covers, i, v->steps[i].at_ms,
                   hex);
            for (uint16_t j = 0u; j < total; j++) {
                const uint8_t *out = fake_port_tx_frame(j, &frame_len);

                r2s_hex_format(out, frame_len > 64u ? 64u : frame_len, hex, sizeof hex);
                printf("%s [%s] produced %u: %s\n", v->where, v->covers, j, hex);
            }
            snprintf(label, sizeof label, "%s [%s] step %u at %ums: no such MCU frame", v->where, v->covers,
                     i, v->steps[i].at_ms);
            TEST_FAIL_MESSAGE(label);
        }
    }
    for (; matched < fake_port_tx_frame_count(); matched++) {
        uint16_t frame_len = 0u;
        const uint8_t *frame = fake_port_tx_frame(matched, &frame_len);
        r2s_hex_format(frame, frame_len > 64u ? 64u : frame_len, hex, sizeof hex);
        printf("%s [%s]: extra MCU frame %s\n", v->where, v->covers, hex);
        produced++;
    }
    printf("%s [%s]: %u of %u MCU steps matched, %u extra frames\n", v->where, v->covers, mcu_steps,
           mcu_steps, produced);
}
