/* Conformance: handshake transcripts through the MCU session. The firmware
 * comes from the transcript: the HELLO names the node and the topic count,
 * and the TOPIC steps fill the table. Host steps go onto the fake port byte
 * by byte at their own times; every MCU step must then come out in order.
 * A frame the peer never receives is reenacted from the tables below: left
 * out of the feed, or matched and logged as lost, with the step and the rule. */
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

/* A frame the peer never receives: the transcript, its time, its channel and,
 * on the control channel, its opcode. A heartbeat carries no opcode, so only
 * the channel matches it. Each entry carries the rule the loss models. */
typedef struct {
    const char *name;
    uint32_t at_ms;
    uint8_t topic;
    uint8_t opcode;
    const char *rule;
} r2s_replay_loss_t;

/* Host frames the MCU never receives: the harness advances the clock through
 * their times but never feeds them. */
static const r2s_replay_loss_t replay_dropped[] = {
    {"topic_retry", 100u, 0u, 0x03u, "SES-16"},      {"link_timeout", 1320u, 4u, 0x00u, "SES-8"},
    {"link_timeout", 1820u, 4u, 0x00u, "SES-8"},     {"link_timeout", 2320u, 4u, 0x00u, "SES-8"},
    {"link_timeout", 2820u, 4u, 0x00u, "SES-8"},     {"reset_same_token", 1320u, 4u, 0x00u, "SES-8"},
    {"reset_same_token", 1820u, 4u, 0x00u, "SES-8"}, {"reset_zero_token", 320u, 0u, 0x06u, "SES-6"},
    {"reset_zero_token", 820u, 4u, 0x00u, "SES-8"},  {"reset_zero_token", 1320u, 4u, 0x00u, "SES-8"},
    {"reset_zero_token", 1820u, 4u, 0x00u, "SES-8"},
};

/* A link the harness expires early: the transcript compresses the silence, so
 * the timeout transition fires at the given time with the token kept. */
typedef struct {
    const char *name;
    uint32_t at_ms;
} r2s_replay_event_t;

static const r2s_replay_event_t replay_expire[] = {
    {"link_timeout", 1000u},
    {"reset_same_token", 1000u},
};

static const r2s_replay_event_t replay_reboot[] = {
    {"reset_zero_token", 2000u},
};

/* One reenactment of each kind per transcript: a second row for the same name
 * would fire the transition twice. */
static void check_single_event(const r2s_replay_event_t *events, uint16_t n, const char *where)
{
    uint16_t i;
    uint16_t j;

    for (i = 0u; i < n; i++) {
        for (j = (uint16_t)(i + 1u); j < n; j++) {
            TEST_ASSERT_NOT_EQUAL_MESSAGE(0, strcmp(events[i].name, events[j].name), where);
        }
    }
}

/* The rule of the reenacted loss the step models, or NULL when the step runs
 * as written. */
static const char *loss_rule(const r2s_replay_loss_t *losses, uint16_t n, const r2s_vec_session_t *v,
                             const r2s_vec_session_step_t *step)
{
    uint16_t i;

    for (i = 0u; i < n; i++) {
        if (strcmp(losses[i].name, v->name) == 0 && losses[i].at_ms == step->at_ms && step->frame_len > 2u &&
            step->frame[0] == losses[i].topic &&
            (losses[i].topic == 4u || step->frame[2] == losses[i].opcode)) {
            return losses[i].rule;
        }
    }
    return NULL;
}

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

/* Fills the node name and topic count from the range's first HELLO, and
 * the table from its TOPIC steps. Anything the firmware cannot take, a second
 * table or a sparse one, fails the test at the step that carries it. */
static void configure_range(const r2s_vec_session_t *v, uint16_t lo, uint16_t hi)
{
    uint8_t topic_count = 0u;
    uint8_t have_hello = 0u;
    uint32_t seen = 0u;
    uint8_t top = 0u;
    char label[256];

    for (uint16_t i = lo; i < hi; i++) {
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

/* The reboot step of the re-announcing transcript: the first MCU HELLO at the
 * reboot time. Anything else there fails the test instead of rebooting at the
 * wrong step. */
static uint16_t reboot_step(const r2s_vec_session_t *v, uint32_t at_ms)
{
    uint16_t found = 0xFFFFu;
    uint16_t i;

    for (i = 0u; i < v->nsteps; i++) {
        const r2s_vec_session_step_t *step = &v->steps[i];

        if (!step->from_host && step->at_ms == at_ms && step->frame_len > 2u && step->frame[0] == 0u &&
            step->frame[2] == 0x02u) {
            TEST_ASSERT_EQUAL_UINT16_MESSAGE(0xFFFFu, found, v->where);
            found = i;
        }
    }
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0xFFFFu, found, v->where);
    return found;
}

/* The firmware of the exhausted walk: its TOPIC answers never reach the wire,
 * so the transcript holds no table. The HELLO matches the topic_retry one
 * byte for byte, whose TOPIC steps source this table. */
static void configure_exhausted(const r2s_vec_session_t *v)
{
    static const uint8_t hash0[8] = {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u};
    static const uint8_t hash1[8] = {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u};

    memcpy(node_name, "node", sizeof "node");
    replay_topics[0].topic_id = 0x10u;
    replay_topics[0].flags = 0x00u;
    memcpy(replay_topics[0].type_hash, hash0, sizeof hash0);
    memcpy(replay_type_names[0], "std_msgs/msg/Float32", sizeof "std_msgs/msg/Float32");
    memcpy(replay_topic_names[0], "battery", sizeof "battery");
    replay_topics[0].type_name = replay_type_names[0];
    replay_topics[0].topic_name = replay_topic_names[0];
    replay_topics[1].topic_id = 0x11u;
    replay_topics[1].flags = 0x05u;
    memcpy(replay_topics[1].type_hash, hash1, sizeof hash1);
    memcpy(replay_type_names[1], "geometry_msgs/msg/Twist", sizeof "geometry_msgs/msg/Twist");
    memcpy(replay_topic_names[1], "cmd_vel", sizeof "cmd_vel");
    replay_topics[1].type_name = replay_type_names[1];
    replay_topics[1].topic_name = replay_topic_names[1];
    TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, r2s_session_configure_ram(node_name, replay_topics, 2u), v->where);
    r2s_init();
}

/* Fills the firmware from the transcript: the whole one, or the first epoch
 * of the transcript that reboots. The reboot finishes the table from the
 * re-announced steps when the replay reaches it. */
static void configure_from_transcript(const r2s_vec_session_t *v)
{
    if (strcmp(v->name, "topic_retry_exhausted") == 0) {
        configure_exhausted(v);
        return;
    }
    if (strcmp(v->name, "reset_zero_token") == 0) {
        configure_range(v, 0u, reboot_step(v, 2000u));
        return;
    }
    configure_range(v, 0u, v->nsteps);
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

/* The TOPIC the exhausted walk announces: the payload the firmware above
 * binds. */
static uint16_t exhausted_topic(uint8_t *out)
{
    static const uint8_t hash[8] = {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u};
    static const char type_name[] = "std_msgs/msg/Float32";
    static const char topic_name[] = "battery";
    uint16_t len = 0u;

    out[len++] = 0x04u;
    out[len++] = 0x00u;
    out[len++] = 0x10u;
    out[len++] = 0x00u;
    memcpy(out + len, hash, sizeof hash);
    len = (uint16_t)(len + sizeof hash);
    out[len++] = (uint8_t)(sizeof type_name - 1u);
    memcpy(out + len, type_name, sizeof type_name - 1u);
    len = (uint16_t)(len + sizeof type_name - 1u);
    out[len++] = (uint8_t)(sizeof topic_name - 1u);
    memcpy(out + len, topic_name, sizeof topic_name - 1u);
    return (uint16_t)(len + sizeof topic_name - 1u);
}

/* A HELLO payload: the fixed fields of the handshake, with the caller's token
 * and topic count, then the node name. The sizes come from the build, so a
 * firmware with other limits fails the replay instead of encoding past them. */
static uint16_t hello_payload(uint8_t *out, uint32_t token, uint8_t topic_count)
{
    static const char node[] = "node";
    uint16_t len = 0u;

    out[len++] = 0x02u;
    out[len++] = 0x02u;
    out[len++] = 0x00u;
    out[len++] = (uint8_t)(token & 0xFFu);
    out[len++] = (uint8_t)((token >> 8u) & 0xFFu);
    out[len++] = (uint8_t)((token >> 16u) & 0xFFu);
    out[len++] = (uint8_t)((token >> 24u) & 0xFFu);
    out[len++] = (uint8_t)(R2S_MAX_FRAME & 0xFFu);
    out[len++] = (uint8_t)((R2S_MAX_FRAME >> 8u) & 0xFFu);
    out[len++] = (uint8_t)(R2S_FRAMER_RX_CAPACITY & 0xFFu);
    out[len++] = (uint8_t)((R2S_FRAMER_RX_CAPACITY >> 8u) & 0xFFu);
    out[len++] = (uint8_t)(R2S_TICK_HZ & 0xFFu);
    out[len++] = (uint8_t)((R2S_TICK_HZ >> 8u) & 0xFFu);
    out[len++] = (uint8_t)((R2S_TICK_HZ >> 16u) & 0xFFu);
    out[len++] = (uint8_t)((R2S_TICK_HZ >> 24u) & 0xFFu);
    out[len++] = 0x00u;
    out[len++] = topic_count;
    out[len++] = (uint8_t)(sizeof node - 1u);
    memcpy(out + len, node, sizeof node - 1u);
    return (uint16_t)(len + sizeof node - 1u);
}

/* The frames the exhausted walk produces past its HELLO: every TOPIC answer
 * goes out but never reaches the wire, and the last HELLO_REQ is answered
 * past the transcript window. Each must be exactly the frame the firmware
 * announces; anything else fails like an extra. Returns the frames consumed. */
static uint16_t verify_exhausted_discards(const r2s_vec_session_t *v, uint8_t *want, char *hex,
                                          size_t hex_len, char *label, size_t label_len, uint16_t matched)
{
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t payload_len;
    uint16_t want_len;
    uint8_t seq;

    for (seq = 1u; seq <= 4u; seq++) {
        const uint8_t *frame = NULL;
        uint16_t frame_len = 0u;

        payload_len = exhausted_topic(payload);
        want_len = r2s_test_frame(want, 0u, seq, payload, payload_len);
        want_len--;
        if (matched < fake_port_tx_frame_count()) {
            frame = fake_port_tx_frame(matched, &frame_len);
            matched++;
        }
        if (frame == NULL || frame_len != want_len || memcmp(frame, want, want_len) != 0) {
            r2s_hex_format(want, want_len > 64u ? 64u : want_len, hex, hex_len);
            printf("%s [%s]: want discarded TOPIC seq %u %s\n", v->where, "SES-2", seq, hex);
            if (frame != NULL) {
                r2s_hex_format(frame, frame_len > 64u ? 64u : frame_len, hex, hex_len);
                printf("%s [%s]: got discarded TOPIC seq %u %s\n", v->where, "SES-2", seq, hex);
            }
            snprintf(label, label_len, "%s: discarded TOPIC seq %u differs", v->where, seq);
            TEST_FAIL_MESSAGE(label);
        }
        printf("%s [SES-16] MCU TOPIC seq %u after host step %u: lost on the wire\n", v->where, seq, seq);
    }
    {
        const uint8_t *frame = NULL;
        uint16_t frame_len = 0u;

        payload_len = hello_payload(payload, 0u, 2u);
        want_len = r2s_test_frame(want, 0u, 5u, payload, payload_len);
        want_len--;
        if (matched < fake_port_tx_frame_count()) {
            frame = fake_port_tx_frame(matched, &frame_len);
            matched++;
        }
        if (frame == NULL || frame_len != want_len || memcmp(frame, want, want_len) != 0) {
            r2s_hex_format(want, want_len > 64u ? 64u : want_len, hex, hex_len);
            printf("%s [%s]: want discarded HELLO seq 5 %s\n", v->where, "SES-2", hex);
            if (frame != NULL) {
                r2s_hex_format(frame, frame_len > 64u ? 64u : frame_len, hex, hex_len);
                printf("%s [%s]: got discarded HELLO seq 5 %s\n", v->where, "SES-2", hex);
            }
            snprintf(label, label_len, "%s: discarded HELLO seq 5 differs", v->where);
            TEST_FAIL_MESSAGE(label);
        }
        printf("%s [SES-14] MCU HELLO seq 5 after host step 5: past the transcript window\n", v->where);
    }
    return matched;
}

/* The repeats the dead link swallows: the session sends them while no host
 * frame arrives, and the host side proves the loss with its own timeout. Each
 * is verified byte for byte and consumed before the step it precedes, naming
 * its time and its rule. Returns the frames consumed. */
static uint16_t drain_lost_repeats(const r2s_vec_session_t *v, uint8_t *want, const uint32_t *prod_ms,
                                   uint16_t matched)
{
    static uint8_t payload[R2S_MAX_FRAME];
    uint8_t seq;

    if (strcmp(v->name, "link_timeout") != 0) {
        return matched;
    }
    for (seq = 0u; seq <= 1u; seq++) {
        const uint8_t *frame = NULL;
        uint16_t frame_len = 0u;
        uint16_t payload_len = hello_payload(payload, 1u, 2u);
        uint16_t want_len = r2s_test_frame(want, 0u, seq, payload, payload_len);

        want_len--;
        if (matched < fake_port_tx_frame_count()) {
            frame = fake_port_tx_frame(matched, &frame_len);
        }
        if (frame == NULL || frame_len != want_len || memcmp(frame, want, want_len) != 0) {
            return matched;
        }
        printf("%s [SES-5] MCU HELLO seq %u at %ums: repeat lost on the dead link\n", v->where, seq,
               prod_ms[matched]);
        matched++;
    }
    return matched;
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

    check_single_event(replay_expire, sizeof replay_expire / sizeof replay_expire[0], v->where);
    check_single_event(replay_reboot, sizeof replay_reboot / sizeof replay_reboot[0], v->where);
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
        uint32_t prev = now;

        if (v->steps[i].at_ms < now) {
            skipped++;
            continue;
        }
        advance_to(&now, v->steps[i].at_ms);
        for (uint16_t e = 0u; e < sizeof replay_expire / sizeof replay_expire[0]; e++) {
            if (strcmp(replay_expire[e].name, v->name) == 0 && replay_expire[e].at_ms > prev &&
                replay_expire[e].at_ms <= now) {
                r2s_session_test_link_timeout();
                printf("%s [SES-7, SES-8] step %u at %ums: the harness expires the link\n", v->where, i, now);
            }
        }
        for (uint16_t e = 0u; e < sizeof replay_reboot / sizeof replay_reboot[0]; e++) {
            if (strcmp(replay_reboot[e].name, v->name) == 0 && replay_reboot[e].at_ms > prev &&
                replay_reboot[e].at_ms <= now) {
                /* configure_range boots with the announced table. */
                r2s_test_reset();
                configure_range(v, reboot_step(v, replay_reboot[e].at_ms), v->nsteps);
                printf("%s [SES-1, SES-10] step %u at %ums: the harness reboots with the announced table\n",
                       v->where, i, now);
            }
        }
        if (v->steps[i].from_host) {
            const char *rule =
                loss_rule(replay_dropped, sizeof replay_dropped / sizeof replay_dropped[0], v, &v->steps[i]);
            if (rule != NULL) {
                printf("%s [%s] step %u at %ums: host frame not fed, lost on the wire\n", v->where, rule, i,
                       v->steps[i].at_ms);
            } else {
                feed_wire(v->steps[i].frame, v->steps[i].frame_len, v->where);
            }
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
        matched = drain_lost_repeats(v, want, prod_ms, matched);
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
    if (strcmp(v->name, "topic_retry_exhausted") == 0) {
        matched = verify_exhausted_discards(v, want, hex, sizeof hex, label, sizeof label, matched);
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
