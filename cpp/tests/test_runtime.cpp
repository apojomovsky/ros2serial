/* The runtime API through the fake port: registration order, the ids and
 * flags the table carries, the publish gates, the dispatch round trip and
 * the errors the closed registration raises. */
#include <float.h>
#include <stdio.h>
#include <string.h>

/* The fake port's header carries no C linkage guard, and this file is C++. */
extern "C" {
#include "fake_port.h"
}
#include "frames.h"
#include "handwritten.h"
#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"
#include "r2s/runtime.hpp"
#include "unity.h"

/* The message types the tests publish and receive are the ones the
 * handwritten fixture describes, so the wire form is the one the core's
 * vectors pin. */
typedef std_msgs__msg__Float32 Float32;
typedef geometry_msgs__msg__Twist Twist;

static const uint8_t s_float32_hash[8] = {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u};
static const uint8_t s_twist_hash[8] = {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u};

namespace r2s
{
template <> struct msg_traits<Float32> {
    static const r2s_codec_table_t *table()
    {
        return &R2S_HANDWRITTEN_TABLE;
    }
    static uint16_t type_index()
    {
        return R2S_HANDWRITTEN_FLOAT32;
    }
    static const char *type_name()
    {
        return "std_msgs/msg/Float32";
    }
    static const uint8_t *type_hash()
    {
        return s_float32_hash;
    }
};

template <> struct msg_traits<Twist> {
    static const r2s_codec_table_t *table()
    {
        return &R2S_HANDWRITTEN_TABLE;
    }
    static uint16_t type_index()
    {
        return R2S_HANDWRITTEN_TWIST;
    }
    static const char *type_name()
    {
        return "geometry_msgs/msg/Twist";
    }
    static const uint8_t *type_hash()
    {
        return s_twist_hash;
    }
};
} // namespace r2s

/* The narrow flag a build with 32-bit doubles carries on every topic
 * ([ENC-12]). */
#if DBL_MANT_DIG == FLT_MANT_DIG
#define NARROW_FLAG 0x02u
#else
#define NARROW_FLAG 0x00u
#endif

/* The id the first declared topic takes; the ids run from 16 in
 * construction order (FRM-3). */
#define FIRST_TOPIC_ID 16u

static uint8_t s_wire[R2S_FRAMER_RX_CAPACITY + 1u];
static Twist s_got_twist;
static uint8_t s_got_calls;
static Twist s_lambda_twist;
static uint8_t s_lambda_calls;

static void on_twist(const Twist &msg)
{
    s_got_twist = msg;
    s_got_calls++;
}

/* Queues bytes one at a time, spinning after each, as a slow link
 * delivers. */
static void feed_slow(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        fake_port_queue_rx(&data[i], 1);
        r2s_spin_some();
    }
}

/* Builds a host frame and feeds it byte by byte. */
static void feed_host(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint16_t wire_len = r2s_test_frame(s_wire, topic_id, seq, payload, len);

    feed_slow(s_wire, wire_len);
}

/* The host's handshake: a hello request, a status per topic and the ready
 * frame that connects the session. */
static void handshake(uint8_t count)
{
    const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
    const uint8_t ready[] = {0x06u, 0x01u, 0x00u, 0x00u, 0x00u};
    uint8_t seq = 0u;
    uint8_t i;

    feed_host(0u, seq++, hello_req, sizeof hello_req);
    for (i = 0u; i < count; i++) {
        uint8_t status[] = {0x05u, (uint8_t)(FIRST_TOPIC_ID + i), 0x00u};

        feed_host(0u, seq++, status, sizeof status);
    }
    feed_host(0u, seq++, ready, sizeof ready);
}

/* The payload of a Twist, encoded the way the codec does. */
static uint16_t twist_payload(uint8_t *out, const Twist &msg, uint8_t narrow)
{
    r2s_codec_out_t sink = {out, R2S_MAX_FRAME, 0u, 0u};
    uint8_t field = 0xFFu;

    TEST_ASSERT_EQUAL_INT8(
        R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_TWIST, &msg, narrow, &sink, &field));
    return sink.len;
}

/* The index-th captured frame against the wire form of its payload. */
static void assert_tx(uint16_t index, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                      uint16_t payload_len)
{
    static uint8_t expected[R2S_FRAMER_RX_CAPACITY + 1u];
    char label[64];
    uint16_t encoded_len;
    const uint8_t *encoded;
    uint16_t expected_len = r2s_test_frame(expected, topic_id, seq, payload, payload_len);

    snprintf(label, sizeof label, "tx frame %u", index);
    encoded = fake_port_tx_frame(index, &encoded_len);
    TEST_ASSERT_NOT_NULL_MESSAGE(encoded, label);
    TEST_ASSERT_EQUAL_UINT16(expected_len - 1u, encoded_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, encoded, expected_len - 1u);
}

/* covers: FRM-3 */
void test_runtime_topics_take_the_ids_from_16_in_construction_order(void)
{
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");
    r2s::Subscription<Twist> cmd_vel_sub(node, "cmd_vel", on_twist);

    node.spin_some();
    TEST_ASSERT_EQUAL_UINT8(2, r2s_topic_count());
    TEST_ASSERT_EQUAL_UINT8(FIRST_TOPIC_ID, r2s_topic_get(0)->topic_id);
    TEST_ASSERT_EQUAL_UINT8(FIRST_TOPIC_ID + 1u, r2s_topic_get(1)->topic_id);
}

/* covers: CTL-4 */
void test_runtime_publisher_and_subscriber_carry_their_direction_flags(void)
{
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");
    r2s::Subscription<Twist> cmd_vel_sub(node, "cmd_vel", on_twist);

    node.spin_some();
    TEST_ASSERT_EQUAL_UINT8(NARROW_FLAG, r2s_topic_get(0)->flags & 0xFEu);
    TEST_ASSERT_EQUAL_UINT8(0x00u, r2s_topic_get(0)->flags & 0x01u);
    TEST_ASSERT_EQUAL_UINT8(0x01u, r2s_topic_get(1)->flags & 0x01u);
}

/* covers: CTL-12 */
void test_runtime_table_announces_the_type_name_and_hash(void)
{
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");

    node.spin_some();
    TEST_ASSERT_EQUAL_STRING("std_msgs/msg/Float32", r2s_topic_get(0)->type_name);
    TEST_ASSERT_EQUAL_STRING("battery", r2s_topic_get(0)->topic_name);
    TEST_ASSERT_EQUAL_MEMORY(s_float32_hash, r2s_topic_get(0)->type_hash, 8u);
}

/* covers: SES-4 */
void test_runtime_publish_before_the_handshake_fails_and_sends_nothing(void)
{
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");
    Float32 battery = {1.5f};

    node.spin_some();
    fake_port_tx_clear();
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, battery_pub.publish(battery));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: FRM-4, FRM-5 */
void test_runtime_publish_sends_the_frame_with_the_topic_sequence(void)
{
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");
    Float32 battery = {1.5f};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    node.spin_some();
    handshake(1u);
    fake_port_tx_clear();
    {
        r2s_codec_out_t sink = {payload, (uint16_t)sizeof payload, 0u, 0u};
        uint8_t field = 0xFFu;

        TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_FLOAT32,
                                                        &battery, 0u, &sink, &field));
        len = sink.len;
    }
    TEST_ASSERT_EQUAL_INT8(R2S_OK, battery_pub.publish(battery));
    assert_tx(0, FIRST_TOPIC_ID, 0u, payload, len);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, battery_pub.publish(battery));
    assert_tx(1, FIRST_TOPIC_ID, 1u, payload, len);
}

void test_runtime_publish_on_a_rejected_topic_fails_and_sends_nothing(void)
{
    const uint8_t status_rejected[] = {0x05u, FIRST_TOPIC_ID, 0x02u};
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");
    Float32 battery = {1.5f};

    node.spin_some();
    handshake(1u);
    feed_host(0u, 6u, status_rejected, sizeof status_rejected);
    fake_port_tx_clear();
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TOPIC_REJECTED, battery_pub.publish(battery));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

void test_runtime_publish_without_space_in_the_port_sends_nothing(void)
{
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");
    Float32 battery = {1.5f};

    node.spin_some();
    handshake(1u);
    fake_port_tx_clear();
    fake_port_set_tx_free(2u);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY, battery_pub.publish(battery));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
}

/* covers: FRM-15 */
void test_runtime_dispatch_calls_the_handler_with_the_decoded_message(void)
{
    r2s::Node node("base_controller");
    r2s::Subscription<Twist> cmd_vel_sub(node, "cmd_vel", on_twist);
    const Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    s_got_calls = 0u;
    node.spin_some();
    handshake(1u);
    len = twist_payload(payload, sent, 0u);
    feed_host(FIRST_TOPIC_ID, 0u, payload, len);
    TEST_ASSERT_EQUAL_UINT8(1, s_got_calls);
    TEST_ASSERT_EQUAL_MEMORY(&sent, &s_got_twist, sizeof sent);
}

void test_runtime_dispatch_on_an_unknown_topic_is_refused(void)
{
    r2s::Node node("base_controller");
    r2s::Subscription<Twist> cmd_vel_sub(node, "cmd_vel", on_twist);
    const Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;
    const uint16_t drops_before = r2s_framer_counters()->dropped_frames;

    s_got_calls = 0u;
    node.spin_some();
    handshake(1u);
    len = twist_payload(payload, sent, 0u);
    feed_host(99u, 0u, payload, len);
    TEST_ASSERT_EQUAL_UINT8(0, s_got_calls);
    TEST_ASSERT_EQUAL_UINT16(drops_before + 1u, r2s_framer_counters()->dropped_frames);
}

/* covers: FRM-15 */
void test_runtime_dispatch_counts_the_lost_frames_of_a_gap(void)
{
    r2s::Node node("base_controller");
    r2s::Subscription<Twist> cmd_vel_sub(node, "cmd_vel", on_twist);
    const Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;
    uint8_t index;

    s_got_calls = 0u;
    node.spin_some();
    handshake(1u);
    len = twist_payload(payload, sent, 0u);
    index = r2s_session_find_topic(FIRST_TOPIC_ID);
    TEST_ASSERT_NOT_EQUAL_UINT8(0xFFu, index);
    feed_host(FIRST_TOPIC_ID, 0u, payload, len);
    feed_host(FIRST_TOPIC_ID, 1u, payload, len);
    feed_host(FIRST_TOPIC_ID, 3u, payload, len);
    TEST_ASSERT_EQUAL_UINT8(3, s_got_calls);
    TEST_ASSERT_EQUAL_UINT16(1, r2s_session_rx_lost(index));
}

void test_runtime_topic_declared_after_the_first_spin_sets_the_error(void)
{
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");

    node.spin_some();
    TEST_ASSERT_FALSE(node.registration_error());
    {
        r2s::Publisher<Float32> late_pub(node, "late");

        (void)late_pub;
    }
    TEST_ASSERT_TRUE(node.registration_error());
    TEST_ASSERT_EQUAL_UINT8(1, r2s_topic_count());
}

/* The sketch's own spelling: a node named node, a publisher and a
 * subscription against it, and a capture-less lambda as the handler. */
static void on_twist_lambda(const Twist &msg)
{
    (void)msg;
    s_got_calls++;
}

void test_runtime_the_sketch_spelling_works(void)
{
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");
    r2s::Subscription<Twist> cmd_vel_sub(node, "cmd_vel", on_twist_lambda);
    Float32 battery = {1.5f};

    s_got_calls = 0u;
    node.spin_some();
    TEST_ASSERT_EQUAL_UINT8(2, r2s_topic_count());
    TEST_ASSERT_EQUAL_UINT8(FIRST_TOPIC_ID, r2s_topic_get(0)->topic_id);
    TEST_ASSERT_EQUAL_UINT8(FIRST_TOPIC_ID + 1u, r2s_topic_get(1)->topic_id);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, battery_pub.publish(battery));
}

void test_runtime_a_capture_less_lambda_is_a_valid_handler(void)
{
    r2s::Node node("base_controller");
    r2s::Subscription<Twist> cmd_vel_sub(node, "cmd_vel", [](const Twist &msg) {
        s_lambda_twist = msg;
        s_lambda_calls++;
    });
    const Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    s_lambda_calls = 0u;
    node.spin_some();
    handshake(1u);
    len = twist_payload(payload, sent, 0u);
    feed_host(FIRST_TOPIC_ID, 0u, payload, len);
    TEST_ASSERT_EQUAL_UINT8(1, s_lambda_calls);
    TEST_ASSERT_EQUAL_MEMORY(&sent, &s_lambda_twist, sizeof sent);
}

/* The port a node begins: the stub stands in for the Arduino stream, so
 * the host build proves the call a sketch makes. */
class StubPort
{
  public:
    void begin(unsigned long baud)
    {
        m_baud = baud;
        m_begun = true;
    }
    unsigned long m_baud = 0UL;
    bool m_begun = false;
};

void test_runtime_begin_starts_the_port_at_the_given_baud(void)
{
    r2s::Node node("base_controller");
    StubPort port;

    node.begin(port, 115200UL);
    TEST_ASSERT_TRUE(port.m_begun);
    TEST_ASSERT_EQUAL_UINT32(115200UL, (uint32_t)port.m_baud);
}

void test_runtime_a_reset_empties_the_registry(void)
{
    {
        r2s::Node node("base_controller");
        r2s::Publisher<Float32> battery_pub(node, "battery");

        node.spin_some();
        TEST_ASSERT_EQUAL_UINT8(1, r2s_topic_count());
    }
    r2s_runtime_test_reset();
    r2s_test_reset();
    {
        r2s::Node node("base_controller");
        r2s::Publisher<Float32> battery_pub(node, "battery");

        node.spin_some();
        TEST_ASSERT_FALSE(node.registration_error());
        TEST_ASSERT_EQUAL_UINT8(1, r2s_topic_count());
        TEST_ASSERT_EQUAL_UINT8(FIRST_TOPIC_ID, r2s_topic_get(0)->topic_id);
    }
}

void test_runtime_second_node_sets_the_error(void)
{
    r2s::Node node("base_controller");
    r2s::Node other("other_controller");

    node.spin_some();
    TEST_ASSERT_TRUE(other.registration_error());
}

void test_runtime_node_reports_the_connection_and_the_tick(void)
{
    r2s::Node node("base_controller");
    r2s::Publisher<Float32> battery_pub(node, "battery");

    node.spin_some();
    TEST_ASSERT_FALSE(node.connected());
    handshake(1u);
    TEST_ASSERT_TRUE(node.connected());
    fake_port_set_tick(1234u);
    TEST_ASSERT_EQUAL_UINT32(1234u, node.now());
}
