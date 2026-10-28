/* Conformance: handshake transcripts through the MCU session. The firmware
 * comes from the transcript: the HELLO names the node and the topic count,
 * and the TOPIC steps fill the table. Host steps go onto the fake port byte
 * by byte at their own times; every MCU step must then come out in order. */
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_varint.h"
#include "unity.h"
#include "vectors.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

/* The largest table the session binds, with room for every name a frame can
 * carry. */
static char node_name[R2S_MAX_FRAME];
static r2s_topic_t replay_topics[R2S_TOPIC_MAX_COUNT];
static char replay_type_names[R2S_TOPIC_MAX_COUNT][R2S_MAX_FRAME];
static char replay_topic_names[R2S_TOPIC_MAX_COUNT][R2S_MAX_FRAME];

/* Copies the varint string at `at` into `out`, NUL-terminated, and returns
 * past its bytes. A truncated string, an overlong one or one with an embedded
 * NUL fails the test instead of configuring a wrong firmware. */
static const uint8_t *parse_string(const uint8_t *at, const uint8_t *end, char *out, const char *label)
{
    uint16_t len;
    uint8_t used;

    TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_VARINT_OK, r2s_varint_decode(at, (uint16_t)(end - at), &len, &used),
                                   label);
    at += used;
    TEST_ASSERT_TRUE_MESSAGE((uint32_t)(end - at) >= len, label);
    TEST_ASSERT_TRUE_MESSAGE((uint32_t)len < (uint32_t)R2S_MAX_FRAME, label);
    memcpy(out, at, len);
    out[len] = '\0';
    TEST_ASSERT_NULL_MESSAGE(memchr(out, '\0', len), label);
    return at + len;
}

/* The payload of one decoded step frame, without its check bytes. */
static const uint8_t *step_payload(const r2s_vec_session_step_t *step, uint16_t *len, const char *label)
{
    TEST_ASSERT_TRUE_MESSAGE(step->frame_len >= 4u, label);
    *len = (uint16_t)(step->frame_len - 4u);
    return step->frame + 2u;
}

/* Fills the node name and topic count from the transcript's first HELLO, and
 * the table from its TOPIC steps. Anything the firmware cannot take, a second
 * table or a sparse one, fails the test at the step that carries it. */
static void configure_from_transcript(const r2s_vec_session_t *v)
{
    uint8_t topic_count = 0u;
    uint8_t have_hello = 0u;
    uint32_t seen = 0u;
    uint8_t top = 0u;
    char label[256];

    for (uint16_t i = 0u; i < v->nsteps; i++) {
        const r2s_vec_session_step_t *step = &v->steps[i];
        const uint8_t *payload;
        uint16_t len;

        snprintf(label, sizeof label, "%s step %u", v->where, i);
        if (step->from_host || step->frame[0] != 0u) {
            continue;
        }
        payload = step_payload(step, &len, label);
        if (len > 0u && payload[0] == 0x02u) {
            const uint8_t *at;
            const uint8_t *end;

            TEST_ASSERT_TRUE_MESSAGE(len >= 18u, label);
            TEST_ASSERT_TRUE_MESSAGE(!have_hello || payload[16] == topic_count, label);
            topic_count = payload[16];
            have_hello = 1u;
            at = payload + 17u;
            end = payload + len;
            TEST_ASSERT_EQUAL_PTR_MESSAGE(end, parse_string(at, end, node_name, label), label);
        } else if (len > 0u && payload[0] == 0x04u) {
            char type_name[R2S_MAX_FRAME];
            char topic_name[R2S_MAX_FRAME];
            uint8_t index;
            const uint8_t *at;
            const uint8_t *end;

            TEST_ASSERT_TRUE_MESSAGE(len >= 14u, label);
            index = payload[1];
            TEST_ASSERT_TRUE_MESSAGE(index < R2S_TOPIC_MAX_COUNT, label);
            at = payload + 12u;
            end = payload + len;
            at = parse_string(at, end, type_name, label);
            TEST_ASSERT_EQUAL_PTR_MESSAGE(end, parse_string(at, end, topic_name, label), label);
            if ((seen & (1u << index)) != 0u) {
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(payload[2], replay_topics[index].topic_id, label);
                TEST_ASSERT_EQUAL_UINT8_MESSAGE(payload[3], replay_topics[index].flags, label);
                TEST_ASSERT_EQUAL_MEMORY_MESSAGE(payload + 4u, replay_topics[index].type_hash, 8u, label);
                TEST_ASSERT_EQUAL_STRING_MESSAGE(type_name, replay_topics[index].type_name, label);
                TEST_ASSERT_EQUAL_STRING_MESSAGE(topic_name, replay_topics[index].topic_name, label);
            } else {
                replay_topics[index].topic_id = payload[2];
                replay_topics[index].flags = payload[3];
                memcpy(replay_topics[index].type_hash, payload + 4u, 8u);
                memcpy(replay_type_names[index], type_name, (size_t)strlen(type_name) + 1u);
                memcpy(replay_topic_names[index], topic_name, (size_t)strlen(topic_name) + 1u);
                replay_topics[index].type_name = replay_type_names[index];
                replay_topics[index].topic_name = replay_topic_names[index];
                seen |= (uint32_t)(1u << index);
                if (index >= top) {
                    top = (uint8_t)(index + 1u);
                }
            }
        }
    }
    snprintf(label, sizeof label, "%s: no HELLO to configure from", v->where);
    TEST_ASSERT_TRUE_MESSAGE(have_hello, label);
    snprintf(label, sizeof label, "%s: the TOPIC steps announce %u topics, the HELLO %u", v->where, top,
             topic_count);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(topic_count, top, label);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)((1u << top) - 1u), seen, label);
    if (top == 0u) {
        TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, r2s_session_configure_ram(node_name, NULL, 0u), v->where);
    } else {
        TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, r2s_session_configure_ram(node_name, replay_topics, top),
                                       v->where);
    }
    r2s_init();
}

/* Moves the tick to at_ms, spinning every millisecond, so the timers fire as
 * the transcript advances. */
static void advance_to(uint32_t *now, uint32_t at_ms)
{
    /* The transcript times milliseconds, the port times ticks. */
    while (*now < at_ms) {
        fake_port_advance((uint32_t)(R2S_TICK_HZ / 1000u));
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
    TEST_ASSERT_TRUE_MESSAGE(decoded_len <= (uint16_t)R2S_MAX_FRAME, label);
    wire_len = r2s_test_frame(wire, decoded[0], decoded[1], decoded + 2u, (uint16_t)(decoded_len - 4u));
    for (uint16_t i = 0u; i < wire_len; i++) {
        fake_port_queue_rx(wire + i, 1u);
        r2s_spin_some();
    }
}

void r2s_vec_check_session(const r2s_vec_session_t *v)
{
    static uint8_t want[WIRE_MAX];
    static uint32_t prod_ms[1024];
    char label[256];
    char hex[R2S_HEX_TEXT_MAX];
    uint32_t now = 0u;
    uint16_t matched = 0u;
    uint16_t produced = 0u;
    uint16_t skipped = 0u;
    uint16_t last_mcu = 0u;

    /* The MCU boots when its first frame goes out: host traffic before that
     * went nowhere, since the port was not open yet (SES-12, SES-13). */
    for (uint16_t i = 0u; i < v->nsteps; i++) {
        if (!v->steps[i].from_host) {
            now = v->steps[i].at_ms;
            break;
        }
    }
    fake_port_set_tick(now * (uint32_t)(R2S_TICK_HZ / 1000u));
    configure_from_transcript(v);
    for (uint16_t i = 0u; i < v->nsteps; i++) {
        if (v->steps[i].at_ms < now) {
            skipped++;
            continue;
        }
        advance_to(&now, v->steps[i].at_ms);
        if (v->steps[i].from_host) {
            feed_wire(v->steps[i].frame, v->steps[i].frame_len, v->where);
        }
        /* Every frame the step produced carries this step's time. */
        TEST_ASSERT_TRUE_MESSAGE(fake_port_tx_frame_count() < 1024u, v->where);
        while (produced < fake_port_tx_frame_count()) {
            prod_ms[produced] = now;
            produced++;
        }
    }
    if (skipped != 0u) {
        printf("%s [%s]: %u pre-boot host steps went nowhere\n", v->where, v->covers, skipped);
    }
    /* Every MCU step must come out in order, no later than its time, and
     * nothing else may: a missing frame and an extra one both fail with
     * the step and the frame. */
    for (uint16_t i = 0u; i < v->nsteps; i++) {
        const uint8_t *frame = NULL;
        uint16_t frame_len = 0u;
        uint16_t want_len;

        if (v->steps[i].from_host) {
            continue;
        }
        last_mcu = i;
        TEST_ASSERT_TRUE_MESSAGE(v->steps[i].frame_len <= (uint16_t)R2S_MAX_FRAME, v->where);
        want_len = r2s_test_frame(want, v->steps[i].frame[0], v->steps[i].frame[1], v->steps[i].frame + 2u,
                                  (uint16_t)(v->steps[i].frame_len - 4u));
        want_len--;
        if (matched < fake_port_tx_frame_count()) {
            frame = fake_port_tx_frame(matched, &frame_len);
            matched++;
        }
        if (frame == NULL || frame_len != want_len || memcmp(frame, want, want_len) != 0) {
            r2s_hex_format(want, want_len > 64u ? 64u : want_len, hex, sizeof hex);
            printf("%s [%s] step %u at %ums: want MCU frame %s\n", v->where, v->covers, i, v->steps[i].at_ms,
                   hex);
            if (frame != NULL) {
                r2s_hex_format(frame, frame_len > 64u ? 64u : frame_len, hex, sizeof hex);
                printf("%s [%s] step %u: got MCU frame %s\n", v->where, v->covers, i, hex);
            } else {
                printf("%s [%s] step %u: the session sent nothing more\n", v->where, v->covers, i);
            }
            snprintf(label, sizeof label, "%s [%s] step %u at %ums: MCU frame differs", v->where, v->covers,
                     i, v->steps[i].at_ms);
            TEST_FAIL_MESSAGE(label);
        }
        if (prod_ms[matched - 1u] > v->steps[i].at_ms) {
            snprintf(label, sizeof label, "%s [%s] step %u at %ums: MCU frame came out at %ums", v->where,
                     v->covers, i, v->steps[i].at_ms, prod_ms[matched - 1u]);
            TEST_FAIL_MESSAGE(label);
        }
    }
    if (matched != fake_port_tx_frame_count()) {
        uint16_t extra = (uint16_t)(fake_port_tx_frame_count() - matched);

        for (; matched < fake_port_tx_frame_count(); matched++) {
            uint16_t frame_len = 0u;
            const uint8_t *frame = fake_port_tx_frame(matched, &frame_len);

            r2s_hex_format(frame, frame_len > 64u ? 64u : frame_len, hex, sizeof hex);
            printf("%s [%s]: extra MCU frame %s\n", v->where, v->covers, hex);
        }
        snprintf(label, sizeof label, "%s [%s]: the session sent %u frames past step %u", v->where, v->covers,
                 extra, last_mcu);
        TEST_FAIL_MESSAGE(label);
    }
}
