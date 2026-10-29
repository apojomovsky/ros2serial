/* The static C++ API over the X-macro list: the node names the session,
 * the rows expand to the global objects, and the generated switch calls
 * each subscription's callback directly. The table below is the real
 * bundled one, so the announce, the publish bytes and the dispatch are the
 * ones a board sends. */
#include <float.h>
#include <stdio.h>
#include <string.h>

/* The fake port's header carries no C linkage guard, and this file is C++. */
extern "C" {
#include "fake_port.h"
}
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"
#include "unity.h"

#define R2S_TOPICS(X)                                                                                        \
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery")                                             \
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_twist)
#include "ros2serial.h"

/* The hooks reset the runtime registry every test; the static binary never
 * links it, so an empty reset satisfies the call. */
extern "C" void r2s_runtime_test_reset(void)
{
}

static const uint8_t s_float32_hash[8] = {0x71u, 0x70u, 0xd3u, 0xd8u, 0xf8u, 0x41u, 0xf7u, 0xbeu};

static geometry_msgs::msg::Twist s_got_twist;
static uint8_t s_got_calls;

void on_twist(const geometry_msgs::msg::Twist &msg)
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
    static uint8_t wire[R2S_FRAMER_RX_CAPACITY + 1u];
    uint16_t wire_len = r2s_test_frame(wire, topic_id, seq, payload, len);

    feed_slow(wire, wire_len);
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
        uint8_t status[] = {0x05u, (uint8_t)(16u + i), 0x00u};

        feed_host(0u, seq++, status, sizeof status);
    }
    feed_host(0u, seq++, ready, sizeof ready);
}

/* The payload of a Twist, encoded the way the codec does. */
static uint16_t twist_payload(uint8_t *out, const geometry_msgs::msg::Twist &msg, uint8_t narrow)
{
    r2s_codec_out_t sink = {out, R2S_MAX_FRAME, 0u, 0u};
    uint8_t field = 0xFFu;

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&r2s_bundled_table_geometry_msgs_Twist,
                                                    (uint16_t)R2S_BUNDLED_INDEX_geometry_msgs_Twist, &msg,
                                                    narrow, &sink, &field));
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

static void begin(const char *name)
{
    static StubPort port;

    r2s::Node node(name);

    node.begin(port, 115200UL);
}

/* covers: SES-1 */
void test_static_begin_configures_the_session_table(void)
{
    begin("base_controller");
    TEST_ASSERT_EQUAL_UINT8(2, r2s_topic_count());
    TEST_ASSERT_EQUAL_UINT8(16u, r2s_topic_get(0)->topic_id);
    TEST_ASSERT_EQUAL_UINT8(17u, r2s_topic_get(1)->topic_id);
}

/* covers: CTL-4 */
void test_static_publisher_and_subscriber_carry_their_direction_flags(void)
{
    begin("base_controller");
    TEST_ASSERT_EQUAL_UINT8(0x00u, r2s_topic_get(0)->flags & 0x01u);
    TEST_ASSERT_EQUAL_UINT8(0x01u, r2s_topic_get(1)->flags & 0x01u);
}

/* covers: CTL-12 */
void test_static_table_announces_the_type_name_and_hash(void)
{
    begin("base_controller");
    TEST_ASSERT_EQUAL_STRING("std_msgs/msg/Float32", r2s_topic_get(0)->type_name);
    TEST_ASSERT_EQUAL_STRING("battery", r2s_topic_get(0)->topic_name);
    TEST_ASSERT_EQUAL_MEMORY(s_float32_hash, r2s_topic_get(0)->type_hash, 8u);
    TEST_ASSERT_EQUAL_STRING("geometry_msgs/msg/Twist", r2s_topic_get(1)->type_name);
    TEST_ASSERT_EQUAL_STRING("cmd_vel", r2s_topic_get(1)->topic_name);
}

/* covers: SES-4 */
void test_static_publish_before_the_handshake_fails_and_sends_nothing(void)
{
    std_msgs::msg::Float32 battery = {1.5f};

    begin("base_controller");
    fake_port_tx_clear();
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, battery_pub.publish(battery));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: FRM-4, FRM-5 */
void test_static_publish_sends_the_frame_with_the_topic_sequence(void)
{
    std_msgs::msg::Float32 battery = {1.5f};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    begin("base_controller");
    handshake(2u);
    fake_port_tx_clear();
    {
        r2s_codec_out_t sink = {payload, (uint16_t)sizeof payload, 0u, 0u};
        uint8_t field = 0xFFu;

        TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&r2s_bundled_table_std_msgs_Float32,
                                                        (uint16_t)R2S_BUNDLED_INDEX_std_msgs_Float32,
                                                        &battery, 0u, &sink, &field));
        len = sink.len;
    }
    TEST_ASSERT_EQUAL_INT8(R2S_OK, battery_pub.publish(battery));
    assert_tx(0, 16u, 0u, payload, len);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, battery_pub.publish(battery));
    assert_tx(1, 16u, 1u, payload, len);
}

void test_static_publish_on_a_rejected_topic_fails_and_sends_nothing(void)
{
    const uint8_t status_rejected[] = {0x05u, 16u, 0x02u};
    std_msgs::msg::Float32 battery = {1.5f};

    begin("base_controller");
    handshake(2u);
    feed_host(0u, 9u, status_rejected, sizeof status_rejected);
    fake_port_tx_clear();
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TOPIC_REJECTED, battery_pub.publish(battery));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

void test_static_publish_without_space_in_the_port_sends_nothing(void)
{
    std_msgs::msg::Float32 battery = {1.5f};

    begin("base_controller");
    handshake(2u);
    fake_port_tx_clear();
    fake_port_set_tx_free(2u);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY, battery_pub.publish(battery));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
}

/* covers: FRM-15 */
void test_static_dispatch_calls_the_handler_with_the_decoded_message(void)
{
    const geometry_msgs::msg::Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    s_got_calls = 0u;
    begin("base_controller");
    handshake(2u);
    len = twist_payload(payload, sent, 0u);
    feed_host(17u, 0u, payload, len);
    TEST_ASSERT_EQUAL_UINT8(1, s_got_calls);
    TEST_ASSERT_EQUAL_MEMORY(&sent, &s_got_twist, sizeof sent);
}

void test_static_dispatch_on_an_unknown_topic_is_refused(void)
{
    const geometry_msgs::msg::Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;
    const uint16_t drops_before = r2s_framer_counters()->dropped_frames;

    s_got_calls = 0u;
    begin("base_controller");
    handshake(2u);
    len = twist_payload(payload, sent, 0u);
    feed_host(99u, 0u, payload, len);
    TEST_ASSERT_EQUAL_UINT8(0, s_got_calls);
    TEST_ASSERT_EQUAL_UINT16(drops_before + 1u, r2s_framer_counters()->dropped_frames);
}

void test_static_begin_starts_the_port_at_the_given_baud(void)
{
    r2s::Node node("base_controller");
    StubPort port;

    node.begin(port, 115200UL);
    TEST_ASSERT_TRUE(port.m_begun);
    TEST_ASSERT_EQUAL_UINT32(115200UL, (uint32_t)port.m_baud);
}

void test_static_connected_tracks_the_handshake(void)
{
    r2s::Node node("base_controller");
    StubPort port;

    node.begin(port, 115200UL);
    TEST_ASSERT_FALSE(node.connected());
    handshake(2u);
    TEST_ASSERT_TRUE(node.connected());
}

void test_static_now_follows_the_port_tick(void)
{
    r2s::Node node("base_controller");
    StubPort port;

    node.begin(port, 115200UL);
    fake_port_set_tick(1500u);
    TEST_ASSERT_EQUAL_UINT32(1500u, node.now());
}
