/* The handwritten firmware through the fake port: publish gating, dispatch,
 * gap counting and the announced narrow flag. */
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "handwritten.h"
#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

static uint8_t wire[WIRE_MAX];

static geometry_msgs__msg__Twist got_cmd_vel;
static uint8_t got_calls;

void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    got_cmd_vel = *msg;
    got_calls++;
}

/* The same announced topics with the narrow flag set, so the tests can use
 * the announced flags rather than the build's own ([CTL-5]). */
static const r2s_topic_t narrow_topics[] = {
    {R2S_HANDWRITTEN_BATTERY_ID,
     0x02u,
     {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u},
     "std_msgs/msg/Float32",
     "battery"},
    {R2S_HANDWRITTEN_CMD_VEL_ID,
     0x03u,
     {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u},
     "geometry_msgs/msg/Twist",
     "cmd_vel"},
    {R2S_HANDWRITTEN_LOG_TEXT_ID,
     0x02u,
     {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u},
     "std_msgs/msg/String",
     "log_text"},
};

static void configure(uint32_t boot_id)
{
    got_calls = 0u;
    memset(&got_cmd_vel, 0, sizeof got_cmd_vel);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    fake_port_set_boot_id(boot_id);
    r2s_init();
}

/* Queues bytes one at a time, spinning after each, as a slow link delivers. */
static void feed_slow(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        fake_port_queue_rx(&data[i], 1);
        r2s_spin_some();
    }
}

/* Builds a host frame and feeds it byte by byte. */
static void feed_host(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint16_t wire_len = r2s_test_frame(wire, topic_id, seq, payload, len);

    feed_slow(wire, wire_len);
}

static void handshake(void)
{
    const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
    const uint8_t topic_req0[] = {0x03u, 0x00u};
    const uint8_t topic_req1[] = {0x03u, 0x01u};
    const uint8_t status_battery[] = {0x05u, R2S_HANDWRITTEN_BATTERY_ID, 0x00u};
    const uint8_t status_cmd_vel[] = {0x05u, R2S_HANDWRITTEN_CMD_VEL_ID, 0x00u};
    const uint8_t ready[] = {0x06u};

    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, topic_req0, sizeof topic_req0);
    feed_host(0, 2, topic_req1, sizeof topic_req1);
    feed_host(0, 3, status_battery, sizeof status_battery);
    feed_host(0, 4, status_cmd_vel, sizeof status_cmd_vel);
    feed_host(0, 5, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
}

/* Encodes a Twist the long way round, so the link tests pin the wiring while
 * the vector test pins the bytes. */
static uint16_t twist_payload(uint8_t *out, const geometry_msgs__msg__Twist *msg, uint8_t narrow)
{
    r2s_codec_out_t sink = {out, R2S_MAX_FRAME, 0u, 0u};
    uint8_t field = 0xFFu;

    TEST_ASSERT_EQUAL_INT8(
        R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_TWIST, msg, narrow, &sink, &field));
    return sink.len;
}

/* Checks the index-th captured frame against the wire form of its payload. */
static void assert_tx(uint16_t index, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                      uint16_t payload_len)
{
    static uint8_t expected[WIRE_MAX];
    char label[64];
    uint16_t encoded_len;
    const uint8_t *encoded;
    uint16_t expected_len = r2s_test_frame(expected, topic_id, seq, payload, payload_len);

    snprintf(label, sizeof label, "tx frame %u", index);
    encoded = fake_port_tx_frame(index, &encoded_len);
    TEST_ASSERT_NOT_NULL_MESSAGE(encoded, label);
    R2S_ASSERT_BYTES(expected, (uint16_t)(expected_len - 1u), encoded, encoded_len, label);
}

/* covers: SES-4 */
void test_codec_publish_while_announcing_fails_and_sends_nothing(void)
{
    std_msgs__msg__Float32 battery = {1.5f};

    configure(0x44332211u);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}

/* covers: FRM-4, FRM-5 */
void test_codec_publish_sends_frames_with_per_topic_sequence(void)
{
    std_msgs__msg__Float32 battery = {1.5f};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    configure(0x44332211u);
    handshake();
    fake_port_tx_clear();
    {
        r2s_codec_out_t sink = {payload, (uint16_t)sizeof payload, 0u, 0u};
        uint8_t field = 0xFFu;

        TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_FLOAT32,
                                                        &battery, 0u, &sink, &field));
        len = sink.len;
    }
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_publish_battery(&battery));
    assert_tx(0, R2S_HANDWRITTEN_BATTERY_ID, 0, payload, len);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_publish_battery(&battery));
    assert_tx(1, R2S_HANDWRITTEN_BATTERY_ID, 1, payload, len);
}

void test_codec_publish_rejected_topic_sends_nothing(void)
{
    const uint8_t status_rejected[] = {0x05u, R2S_HANDWRITTEN_BATTERY_ID, 0x02u};
    std_msgs__msg__Float32 battery = {1.5f};

    configure(0x44332211u);
    handshake();
    feed_host(0, 6, status_rejected, sizeof status_rejected);
    fake_port_tx_clear();
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TOPIC_REJECTED, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

void test_codec_publish_unconfigured_topic_sends_nothing(void)
{
    static const r2s_topic_t sub_only[] = {
        {R2S_HANDWRITTEN_CMD_VEL_ID, 0x01u, {0x10u}, "geometry_msgs/msg/Twist", "cmd_vel"},
    };
    std_msgs__msg__Float32 battery = {1.5f};

    got_calls = 0u;
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", sub_only, 1u));
    fake_port_set_boot_id(0x44332211u);
    r2s_init();
    handshake();
    fake_port_tx_clear();
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TOPIC_REJECTED, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: DSC-35 */
void test_codec_publish_bound_violation_sends_nothing(void)
{
    std_msgs__msg__String text;
    const uint8_t status_log_text[] = {0x05u, R2S_HANDWRITTEN_LOG_TEXT_ID, 0x00u};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    configure(0x44332211u);
    handshake();
    feed_host(0, 6, status_log_text, sizeof status_log_text);
    memset(&text, 0, sizeof text);
    memcpy(text.data, "hi", 3u);
    {
        r2s_codec_out_t sink = {payload, (uint16_t)sizeof payload, 0u, 0u};
        uint8_t field = 0xFFu;

        TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_STRING, &text,
                                                        0u, &sink, &field));
        len = sink.len;
    }
    fake_port_tx_clear();
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_publish_log_text(&text));
    assert_tx(0, R2S_HANDWRITTEN_LOG_TEXT_ID, 0, payload, len);
    memset(text.data, 'x', sizeof text.data);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_publish_log_text(&text));
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}

void test_codec_publish_without_space_sends_nothing(void)
{
    std_msgs__msg__Float32 battery = {1.5f};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    configure(0x44332211u);
    handshake();
    {
        r2s_codec_out_t sink = {payload, (uint16_t)sizeof payload, 0u, 0u};
        uint8_t field = 0xFFu;

        TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_FLOAT32,
                                                        &battery, 0u, &sink, &field));
        len = sink.len;
    }
    fake_port_tx_clear();
    fake_port_set_tx_free(9u);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_publish_battery(&battery));
    assert_tx(0, R2S_HANDWRITTEN_BATTERY_ID, 0, payload, len);
}

/* The session returns the flags it was configured with, which publish and
 * dispatch read for the narrow decision. */
void test_codec_announced_flags_carry_narrow_on_32_bit_doubles(void)
{
    uint8_t index;

    configure(0x44332211u);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(DBL_MANT_DIG == FLT_MANT_DIG), R2S_HANDWRITTEN_NARROW != 0u);
    index = r2s_session_find_topic(R2S_HANDWRITTEN_BATTERY_ID);
    TEST_ASSERT_NOT_EQUAL_UINT8(0xFFu, index);
    TEST_ASSERT_EQUAL_UINT8(R2S_HANDWRITTEN_NARROW, r2s_session_topic_flags(index) & 0x02u);
    index = r2s_session_find_topic(R2S_HANDWRITTEN_CMD_VEL_ID);
    TEST_ASSERT_NOT_EQUAL_UINT8(0xFFu, index);
    TEST_ASSERT_EQUAL_UINT8(R2S_HANDWRITTEN_NARROW, r2s_session_topic_flags(index) & 0x02u);
}

void test_codec_subscribe_dispatches_to_the_handler(void)
{
    static const geometry_msgs__msg__Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    configure(0x44332211u);
    handshake();
    len = twist_payload(payload, &sent, 0u);
    TEST_ASSERT_EQUAL_UINT16(48, len);
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 0, payload, len);
    TEST_ASSERT_EQUAL_UINT8(1, got_calls);
    TEST_ASSERT_EQUAL_MEMORY(&sent, &got_cmd_vel, sizeof sent);
}

/* covers: FRM-15 */
void test_codec_gap_counts_lost_frames_without_dropping(void)
{
    static const geometry_msgs__msg__Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;
    uint8_t index;

    configure(0x44332211u);
    handshake();
    len = twist_payload(payload, &sent, 0u);
    index = r2s_session_find_topic(R2S_HANDWRITTEN_CMD_VEL_ID);
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 0, payload, len);
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 1, payload, len);
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 3, payload, len);
    TEST_ASSERT_EQUAL_UINT8(3, got_calls);
    TEST_ASSERT_EQUAL_UINT16(1, r2s_session_rx_lost(index));
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 4, payload, len);
    TEST_ASSERT_EQUAL_UINT8(4, got_calls);
    TEST_ASSERT_EQUAL_UINT16(1, r2s_session_rx_lost(index));
}

/* covers: FRM-15, SES-11 */
void test_codec_sequence_restarts_after_each_handshake(void)
{
    static const geometry_msgs__msg__Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
    const uint8_t status_battery[] = {0x05u, R2S_HANDWRITTEN_BATTERY_ID, 0x00u};
    const uint8_t status_cmd_vel[] = {0x05u, R2S_HANDWRITTEN_CMD_VEL_ID, 0x00u};
    const uint8_t ready[] = {0x06u};
    uint16_t len;
    uint8_t index;

    configure(0x44332211u);
    handshake();
    len = twist_payload(payload, &sent, 0u);
    index = r2s_session_find_topic(R2S_HANDWRITTEN_CMD_VEL_ID);
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 250, payload, len);
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 255, payload, len);
    TEST_ASSERT_EQUAL_UINT16(254, r2s_session_rx_lost(index));
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 0, payload, len);
    TEST_ASSERT_EQUAL_UINT16(254, r2s_session_rx_lost(index));
    TEST_ASSERT_EQUAL_UINT8(3, got_calls);
    feed_host(0, 6, hello_req, sizeof hello_req);
    feed_host(0, 7, status_battery, sizeof status_battery);
    feed_host(0, 8, status_cmd_vel, sizeof status_cmd_vel);
    feed_host(0, 9, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 5, payload, len);
    TEST_ASSERT_EQUAL_UINT8(4, got_calls);
    TEST_ASSERT_EQUAL_UINT16(259, r2s_session_rx_lost(index));
}

/* covers: CTL-5 */
void test_codec_narrow_uses_the_announced_flags(void)
{
    static const geometry_msgs__msg__Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len = twist_payload(payload, &sent, 1u);
    const uint16_t drops_before = r2s_framer_counters()->dropped_frames;

    TEST_ASSERT_EQUAL_UINT16(24, len);
    configure(0x44332211u);
    handshake();
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 0, payload, len);
    TEST_ASSERT_EQUAL_UINT8(0, got_calls);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(drops_before + 1u), r2s_framer_counters()->dropped_frames);

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", narrow_topics, 3u));
    handshake();
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 0, payload, len);
    TEST_ASSERT_EQUAL_UINT8(1, got_calls);
    TEST_ASSERT_EQUAL_MEMORY(&sent, &got_cmd_vel, sizeof sent);
}

/* covers: DSC-36 */
void test_codec_decode_failure_drops_without_calling_the_handler(void)
{
    static const geometry_msgs__msg__Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME + 1u];
    uint16_t len = twist_payload(payload, &sent, 0u);
    const uint16_t drops_before = r2s_framer_counters()->dropped_frames;

    payload[len] = 0xFFu;
    configure(0x44332211u);
    handshake();
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 0, payload, (uint16_t)(len + 1u));
    TEST_ASSERT_EQUAL_UINT8(0, got_calls);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(drops_before + 1u), r2s_framer_counters()->dropped_frames);
}

void test_codec_frame_on_unknown_or_publish_topic_is_refused(void)
{
    static const geometry_msgs__msg__Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len = twist_payload(payload, &sent, 0u);
    const uint16_t drops_before = r2s_framer_counters()->dropped_frames;

    configure(0x44332211u);
    handshake();
    feed_host(19, 0, payload, len);
    feed_host(R2S_HANDWRITTEN_BATTERY_ID, 0, payload, len);
    TEST_ASSERT_EQUAL_UINT8(0, got_calls);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(drops_before + 2u), r2s_framer_counters()->dropped_frames);
}
