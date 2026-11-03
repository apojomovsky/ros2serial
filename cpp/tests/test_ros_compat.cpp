/* The rosserial-compatible API over the runtime registry: the sketch names its
 * topics the rosserial way, and the announce, the publish bytes and the
 * dispatch are the ones the runtime layer speaks. The bundled tables carry
 * the wire form, so no handwritten fixture stands in for a message. */
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
#include "ros2serial/geometry_msgs/Twist.h"
#include "ros2serial/ros.h"
#include "ros2serial/std_msgs/Float32.h"
#include "ros2serial/std_msgs/String.h"
#include "unity.h"

/* The compatibility aliases name the bundled types, so a sketch written
 * against ros_lib spells the same types the runtime layer encodes. */
static_assert(sizeof(std_msgs::Float32) == sizeof(std_msgs::msg::Float32), "alias");
static_assert(sizeof(geometry_msgs::Twist) == sizeof(geometry_msgs::msg::Twist), "alias");

static uint8_t s_wire[R2S_FRAMER_RX_CAPACITY + 1u];

/* The bundled type hashes the announce must carry, as the bundle pins
 * them: comparing against the traits would re-derive a miscapture. */
static const uint8_t s_float32_hash[8] = {0x71u, 0x70u, 0xd3u, 0xd8u, 0xf8u, 0x41u, 0xf7u, 0xbeu};
static const uint8_t s_twist_hash[8] = {0x9cu, 0x45u, 0xbfu, 0x16u, 0xfeu, 0x09u, 0x83u, 0xd8u};

static geometry_msgs::Twist s_got_twist;
static uint8_t s_got_calls;

static void on_twist(const geometry_msgs::Twist &msg)
{
    s_got_twist = msg;
    s_got_calls++;
}

static uint8_t s_got_string_calls;

static void on_string(const std_msgs::String &msg)
{
    (void)msg;
    s_got_string_calls++;
}

/* A member-function callback, the Subscriber<T, ObjT> arity. */
class Echo
{
  public:
    void on_twist(const geometry_msgs::Twist &msg)
    {
        m_got = msg;
        m_calls++;
    }
    geometry_msgs::Twist m_got;
    uint8_t m_calls = 0u;
};

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
        uint8_t status[] = {0x05u, (uint8_t)(16u + i), 0x00u};

        feed_host(0u, seq++, status, sizeof status);
    }
    feed_host(0u, seq++, ready, sizeof ready);
}

/* The payload of a Twist, encoded the way the codec does. */
static uint16_t twist_payload(uint8_t *out, const geometry_msgs::Twist &msg, uint8_t narrow)
{
    r2s_codec_out_t sink = {out, R2S_MAX_FRAME, 0u, 0u};
    uint8_t field = 0xFFu;

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&r2s::r2s_bundled_table_geometry_msgs_Twist,
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

/* A sketch's setup: the objects live at file scope in a sketch, and here
 * they live in the test so each test registers from an empty registry. */
struct Sketch {
    ros::NodeHandle nh;
    ros::Publisher battery_pub;
    ros::Subscriber<geometry_msgs::Twist> cmd_vel_sub;
    std_msgs::Float32 battery;

    Sketch() : battery_pub("battery", &battery), cmd_vel_sub("cmd_vel", &on_twist)
    {
        battery.data = 0.0f;
    }

    void setup()
    {
        nh.initNode();
        TEST_ASSERT_TRUE(nh.advertise(battery_pub));
        TEST_ASSERT_TRUE(nh.subscribe(cmd_vel_sub));
    }
};

void test_ros_advertise_registers_topics_in_construction_order(void)
{
    Sketch sketch;

    sketch.setup();
    TEST_ASSERT_EQUAL_INT(ros::SPIN_OK, sketch.nh.spinOnce());
    TEST_ASSERT_EQUAL_UINT8(2, r2s_topic_count());
    TEST_ASSERT_EQUAL_UINT8(16u, r2s_topic_get(0)->topic_id);
    TEST_ASSERT_EQUAL_UINT8(17u, r2s_topic_get(1)->topic_id);
    TEST_ASSERT_EQUAL_STRING("battery", r2s_topic_get(0)->topic_name);
    TEST_ASSERT_EQUAL_STRING("cmd_vel", r2s_topic_get(1)->topic_name);
    TEST_ASSERT_EQUAL_STRING("std_msgs/msg/Float32", r2s_topic_get(0)->type_name);
    TEST_ASSERT_EQUAL_STRING("geometry_msgs/msg/Twist", r2s_topic_get(1)->type_name);
    TEST_ASSERT_EQUAL_MEMORY(s_float32_hash, r2s_topic_get(0)->type_hash, 8u);
    TEST_ASSERT_EQUAL_MEMORY(s_twist_hash, r2s_topic_get(1)->type_hash, 8u);
    TEST_ASSERT_EQUAL_UINT8(0x00u, r2s_topic_get(0)->flags & 0x01u);
    TEST_ASSERT_EQUAL_UINT8(0x01u, r2s_topic_get(1)->flags & 0x01u);
    TEST_ASSERT_EQUAL_INT(0, sketch.battery_pub.getEndpointType());
    TEST_ASSERT_EQUAL_INT(1, sketch.cmd_vel_sub.getEndpointType());
}

/* The size parameters ride along for sketches written against ros_lib and
 * change nothing: the table is the runtime's fixed RAM table either way. */
void test_ros_size_parameters_are_accepted_and_ignored(void)
{
    ros::NodeHandle_<ros::ArduinoHardware, 1, 1, 64, 64> nh;
    std_msgs::Float32 battery;
    ros::Publisher battery_pub("battery", &battery);

    battery.data = 0.0f;
    nh.initNode();
    TEST_ASSERT_TRUE(nh.advertise(battery_pub));
    TEST_ASSERT_EQUAL_INT(ros::SPIN_OK, nh.spinOnce());
    TEST_ASSERT_EQUAL_UINT8(1, r2s_topic_count());
    TEST_ASSERT_EQUAL_UINT8(16u, r2s_topic_get(0)->topic_id);
}

void test_ros_publish_sends_the_frame_with_the_topic_sequence(void)
{
    Sketch sketch;
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    sketch.setup();
    sketch.nh.spinOnce();
    handshake(2u);
    fake_port_tx_clear();
    sketch.battery.data = 1.5f;
    TEST_ASSERT_EQUAL_INT(0, sketch.battery_pub.publish(&sketch.battery));
    {
        r2s_codec_out_t sink = {payload, (uint16_t)sizeof payload, 0u, 0u};
        uint8_t field = 0xFFu;

        TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&r2s::r2s_bundled_table_std_msgs_Float32,
                                                        (uint16_t)R2S_BUNDLED_INDEX_std_msgs_Float32,
                                                        &sketch.battery, 0u, &sink, &field));
        len = sink.len;
    }
    assert_tx(0, 16u, 0u, payload, len);
    TEST_ASSERT_EQUAL_INT(0, sketch.battery_pub.publish(&sketch.battery));
    assert_tx(1, 16u, 1u, payload, len);
}

/* Publishing before the handshake connects nothing, so the call reports
 * the failure instead of sending: code 5, battery, one occurrence. */
void test_ros_publish_before_the_handshake_reports_failure(void)
{
    Sketch sketch;
    const uint8_t report[] = {0x05u, 16u, 0x01u, 0x00u, 0xFFu};

    sketch.setup();
    sketch.nh.spinOnce();
    fake_port_tx_clear();
    TEST_ASSERT_EQUAL_INT(-1, sketch.battery_pub.publish(&sketch.battery));
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_tx(0, 5u, 0u, report, sizeof report);
}

void test_ros_dispatch_calls_the_handler_with_the_decoded_message(void)
{
    Sketch sketch;
    const geometry_msgs::Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    s_got_calls = 0u;
    sketch.setup();
    sketch.nh.spinOnce();
    handshake(2u);
    len = twist_payload(payload, sent, 0u);
    feed_host(17u, 0u, payload, len);
    TEST_ASSERT_EQUAL_UINT8(1, s_got_calls);
    TEST_ASSERT_EQUAL_MEMORY(&sent, &s_got_twist, sizeof sent);
}

void test_ros_member_function_subscriber_dispatches_to_the_object(void)
{
    ros::NodeHandle nh;
    Echo echo;
    ros::Subscriber<geometry_msgs::Twist, Echo> cmd_vel_sub("cmd_vel", &Echo::on_twist, &echo);
    const geometry_msgs::Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    static uint8_t payload[R2S_MAX_FRAME];
    uint16_t len;

    echo.m_calls = 0u;
    nh.initNode();
    TEST_ASSERT_TRUE(nh.subscribe(cmd_vel_sub));
    nh.spinOnce();
    handshake(1u);
    len = twist_payload(payload, sent, 0u);
    feed_host(16u, 0u, payload, len);
    TEST_ASSERT_EQUAL_UINT8(1, echo.m_calls);
    TEST_ASSERT_EQUAL_MEMORY(&sent, &echo.m_got, sizeof sent);
}

/* covers: ERR-4 */
void test_ros_dispatch_reports_a_bound_violation_with_its_field(void)
{
    ros::NodeHandle nh;
    ros::Subscriber<std_msgs::String> log_text_sub("log_text", &on_string);
    /* Seventeen characters against the bundled bound of sixteen. */
    const uint8_t overlong[] = {0x11u, '1', '2', '3', '4', '5', '6', '7', '8',
                                '9',   'a', 'b', 'c', 'd', 'e', 'f', 'g'};
    /* code 1, the subscribed topic, one occurrence, field 0. */
    const uint8_t report[] = {0x01u, 16u, 0x01u, 0x00u, 0x00u};

    s_got_string_calls = 0u;
    nh.initNode();
    TEST_ASSERT_TRUE(nh.subscribe(log_text_sub));
    nh.spinOnce();
    handshake(1u);
    fake_port_tx_clear();
    feed_host(16u, 0u, overlong, sizeof overlong);
    TEST_ASSERT_EQUAL_UINT8(0, s_got_string_calls);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_tx(0, 5u, 0u, report, sizeof report);
}

/* The table closes with the first spin, so a topic advertised after that
 * is refused instead of announced. */
void test_ros_advertise_after_the_first_spin_is_refused(void)
{
    Sketch sketch;
    std_msgs::Float32 late_msg;
    ros::Publisher late_pub("late", &late_msg);

    late_msg.data = 0.0f;
    sketch.setup();
    sketch.nh.spinOnce();
    TEST_ASSERT_FALSE(sketch.nh.advertise(late_pub));
    TEST_ASSERT_EQUAL_UINT8(2, r2s_topic_count());
}
