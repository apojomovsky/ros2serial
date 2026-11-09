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
#include "ros2serial/tf/transform_broadcaster.h"
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

static void on_twist(const geometry_msgs::Twist & msg)
{
  s_got_twist = msg;
  s_got_calls++;
}

static uint8_t s_got_string_calls;

static void on_string(const std_msgs::String & msg)
{
  (void)msg;
  s_got_string_calls++;
}

/* A member-function callback, the Subscriber<T, ObjT> arity. */
class Echo
{
public:
  void on_twist(const geometry_msgs::Twist & msg)
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
static uint16_t twist_payload(uint8_t *out, const geometry_msgs::Twist & msg, uint8_t narrow)
{
  r2s_codec_out_t sink = {out, R2S_MAX_FRAME, 0u, 0u};
  uint8_t field = 0xFFu;

  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&r2s::r2s_bundled_table_geometry_msgs_Twist,
    (uint16_t)R2S_BUNDLED_INDEX_geometry_msgs_Twist, &msg,
                                                    narrow, &sink, &field));
  return sink.len;
}

/* The index-th captured frame against the wire form of its payload. */
static void assert_tx(
  uint16_t index, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
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
struct Sketch
{
  ros::NodeHandle nh;
  ros::Publisher battery_pub;
  ros::Subscriber<geometry_msgs::Twist> cmd_vel_sub;
  std_msgs::Float32 battery;

  Sketch()
  : battery_pub("battery", &battery), cmd_vel_sub("cmd_vel", &on_twist)
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

/* A member-function string subscriber for the bounds case. */
class StringEcho
{
public:
  void on_string(const std_msgs::String & msg)
  {
    (void)msg;
    m_calls++;
  }
  uint8_t m_calls = 0u;
};

/* covers: ERR-4 */
void test_ros_member_dispatch_reports_a_bound_violation_with_its_field(void)
{
  ros::NodeHandle nh;
  StringEcho echo;
  ros::Subscriber<std_msgs::String, StringEcho> log_text_sub("log_text", &StringEcho::on_string,
    &echo);
    /* Seventeen characters against the bundled bound of sixteen. */
  const uint8_t overlong[] = {0x11u, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', 'a', 'b', 'c', 'd', 'e', 'f', 'g'};
    /* code 1, the subscribed topic, one occurrence, field 0. */
  const uint8_t report[] = {0x01u, 16u, 0x01u, 0x00u, 0x00u};

  echo.m_calls = 0u;
  nh.initNode();
  TEST_ASSERT_TRUE(nh.subscribe(log_text_sub));
  nh.spinOnce();
  handshake(1u);
  fake_port_tx_clear();
  feed_host(16u, 0u, overlong, sizeof overlong);
  TEST_ASSERT_EQUAL_UINT8(0, echo.m_calls);
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 5u, 0u, report, sizeof report);
}

/* covers: ERR-4 */
void test_ros_dispatch_reports_a_bound_violation_with_its_field(void)
{
  ros::NodeHandle nh;
  ros::Subscriber<std_msgs::String> log_text_sub("log_text", &on_string);
    /* Seventeen characters against the bundled bound of sixteen. */
  const uint8_t overlong[] = {0x11u, '1', '2', '3', '4', '5', '6', '7', '8',
    '9', 'a', 'b', 'c', 'd', 'e', 'f', 'g'};
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

/* connected() tracks the handshake: false after the first spin, true once
 * the ready frame completes it. */
void test_ros_connected_tracks_the_handshake(void)
{
  Sketch sketch;

  sketch.setup();
  sketch.nh.spinOnce();
  TEST_ASSERT_FALSE(sketch.nh.connected());
  handshake(2u);
  TEST_ASSERT_TRUE(sketch.nh.connected());
}

/* now() reads the shared clock, so it is {0, 0} while no correction
 * anchors it. */
void test_ros_now_is_zero_while_time_is_unsynced(void)
{
  ros::NodeHandle nh;
  ros::Time t = nh.now();

  TEST_ASSERT_EQUAL_UINT32(0u, t.sec);
  TEST_ASSERT_EQUAL_UINT32(0u, t.nsec);
}

/* A host correction anchors the clock, and now() is the anchor plus the
 * ticks since it, through the same arithmetic r2s_now() speaks. */
void test_ros_now_follows_the_shared_clock(void)
{
  ros::NodeHandle nh;
  ros::Time t;

  nh.initNode();
  r2s_time_note_correction(1000u, 1700000000, 500000000u, 0);
  fake_port_set_tick(1250u);
  t = nh.now();
  TEST_ASSERT_EQUAL_UINT32(1700000000u, t.sec);
  TEST_ASSERT_EQUAL_UINT32(750000000u, t.nsec);
}

/* A spin that stays inside the timeout reports SPIN_OK. Zero disables the
 * bound, as in ros_lib. */
void test_ros_spin_without_timeout_serves_the_link(void)
{
  Sketch sketch;
  const geometry_msgs::Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
  static uint8_t payload[R2S_MAX_FRAME];
  uint16_t len;

  s_got_calls = 0u;
  sketch.setup();
  sketch.nh.spinOnce();
  handshake(2u);
  sketch.nh.setSpinTimeout(0u);
  len = twist_payload(payload, sent, 0u);
  feed_host(17u, 0u, payload, len);
  TEST_ASSERT_EQUAL_INT(ros::SPIN_OK, sketch.nh.spinOnce());
  TEST_ASSERT_EQUAL_UINT8(1, s_got_calls);
}

/* A callback that spends time: it moves the fake clock past the spin
 * timeout, so the spin that dispatches it overruns deterministically. */
static void on_slow_twist(const geometry_msgs::Twist & msg)
{
  (void)msg;
  fake_port_advance(100u);
  s_got_calls++;
}

/* A spin that runs past its timeout reports SPIN_TIMEOUT, after serving
 * the frame that spent the time. */
void test_ros_spin_past_the_timeout_reports_the_overrun(void)
{
  ros::NodeHandle nh;
  ros::Subscriber<geometry_msgs::Twist> cmd_vel_sub("cmd_vel", &on_slow_twist);
  const geometry_msgs::Twist sent = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
  static uint8_t payload[R2S_MAX_FRAME];
  static uint8_t wire[R2S_FRAMER_RX_CAPACITY + 1u];
  uint16_t len;
  uint16_t wire_len;

  s_got_calls = 0u;
  nh.initNode();
  TEST_ASSERT_TRUE(nh.subscribe(cmd_vel_sub));
  nh.spinOnce();
  handshake(1u);
  len = twist_payload(payload, sent, 0u);
  wire_len = r2s_test_frame(wire, 16u, 0u, payload, len);
  fake_port_queue_rx(wire, wire_len);
  nh.setSpinTimeout(10u);
  TEST_ASSERT_EQUAL_INT(ros::SPIN_TIMEOUT, nh.spinOnce());
  TEST_ASSERT_EQUAL_UINT8(1, s_got_calls);
}

/* getHardware() exposes the node's own hardware instance, of the template
 * type the sketch names. */
struct StubHardware
{
  uint8_t tag = 7u;
};

void test_ros_get_hardware_returns_the_template_hardware(void)
{
  ros::NodeHandle_<StubHardware> nh;

  TEST_ASSERT_EQUAL_UINT8(7u, nh.getHardware()->tag);
  TEST_ASSERT_EQUAL_PTR(nh.getHardware(), nh.getHardware());
}

/* Each log call sends one frame on the log channel with its level, the
 * text and no value. */
void test_ros_log_routes_each_level_to_the_log_channel(void)
{
  typedef void (ros::NodeHandle::*log_fn)(const char *);
  static const log_fn calls[] = {&ros::NodeHandle::logdebug, &ros::NodeHandle::loginfo,
    &ros::NodeHandle::logwarn, &ros::NodeHandle::logerror,
    &ros::NodeHandle::logfatal};
  static const uint8_t levels[] = {10u, 20u, 30u, 40u, 50u};
  Sketch sketch;
  uint8_t i;

  sketch.setup();
  sketch.nh.spinOnce();
  fake_port_tx_clear();
  for (i = 0u; i < 5u; i++) {
    (sketch.nh.*calls[i])("hi");
  }
  for (i = 0u; i < 5u; i++) {
    const uint8_t payload[] = {levels[i], 0x02u, 'h', 'i', 0x00u};

    assert_tx(i, 2u, i, payload, sizeof payload);
  }
}

/* ros::Time carries sec and nsec and normalizes, as in ros_lib. */
void test_ros_time_normalizes_nanoseconds_in_the_constructor(void)
{
  ros::Time t(1u, 2500000000u);

  TEST_ASSERT_EQUAL_UINT32(3u, t.sec);
  TEST_ASSERT_EQUAL_UINT32(500000000u, t.nsec);
}

/* toSec and fromSec convert through seconds, as in ros_lib. */
void test_ros_time_converts_seconds(void)
{
  ros::Time t;

  t.fromSec(1.5);
  TEST_ASSERT_EQUAL_UINT32(1u, t.sec);
  TEST_ASSERT_EQUAL_UINT32(500000000u, t.nsec);
  TEST_ASSERT_FLOAT_WITHIN(0.000001f, 1.5f, (float)t.toSec());
}

/* toNsec and fromNSec convert through nanoseconds, as in ros_lib. */
void test_ros_time_converts_nanoseconds(void)
{
  ros::Time t;
  ros::Time fixed(1u, 500000000u);

  t.fromNSec(1500000000);
  TEST_ASSERT_EQUAL_UINT32(1u, t.sec);
  TEST_ASSERT_EQUAL_UINT32(500000000u, t.nsec);
  TEST_ASSERT_EQUAL_UINT32(1500000000u, fixed.toNsec());
}

/* Time gains and loses whole durations, as in ros_lib. */
void test_ros_time_adds_and_subtracts_durations(void)
{
  ros::Time t(10u, 800000000u);

  t += ros::Duration(5, 500000000);
  TEST_ASSERT_EQUAL_UINT32(16u, t.sec);
  TEST_ASSERT_EQUAL_UINT32(300000000u, t.nsec);
  t -= ros::Duration(5, 500000000);
  TEST_ASSERT_EQUAL_UINT32(10u, t.sec);
  TEST_ASSERT_EQUAL_UINT32(800000000u, t.nsec);
}

/* The difference of two times is the signed duration between them. */
void test_ros_time_difference_is_a_duration(void)
{
  ros::Time then(10u, 200000000u);
  ros::Time now(7u, 800000000u);
  ros::Duration d = then - now;

  TEST_ASSERT_EQUAL_INT32(2, d.sec);
  TEST_ASSERT_EQUAL_INT32(400000000, d.nsec);
}

/* Durations add, subtract and scale, as in ros_lib. */
void test_ros_duration_arithmetic_matches_ros_lib(void)
{
  ros::Duration d(1, 900000000);

  d += ros::Duration(0, 200000000);
  TEST_ASSERT_EQUAL_INT32(2, d.sec);
  TEST_ASSERT_EQUAL_INT32(100000000, d.nsec);
  d -= ros::Duration(2, 50000000);
  TEST_ASSERT_EQUAL_INT32(0, d.sec);
  TEST_ASSERT_EQUAL_INT32(50000000, d.nsec);
  d.fromSec(0.25);
  TEST_ASSERT_EQUAL_INT32(0, d.sec);
  TEST_ASSERT_EQUAL_INT32(250000000, d.nsec);
  d *= 2.0;
  TEST_ASSERT_EQUAL_INT32(0, d.sec);
  TEST_ASSERT_EQUAL_INT32(500000000, d.nsec);
}

/* The TFMessage hash the announce must carry, as the bundle pins it: the
 * bridge resolves the /tf topic against the installed tf2_msgs support. */
static const uint8_t s_tf_message_hash[8] = {0xe3u, 0x69u, 0xd0u, 0xf0u, 0x5au, 0x23u, 0xaeu,
  0x52u};

/* init() announces the broadcaster's publisher on /tf, carrying the
 * TFMessage type, as in ros_lib. */
void test_ros_broadcaster_init_advertises_tf_topic(void)
{
  ros::NodeHandle nh;
  tf::TransformBroadcaster broadcaster;

  nh.initNode();
  broadcaster.init(nh);
  TEST_ASSERT_EQUAL_INT(ros::SPIN_OK, nh.spinOnce());
  TEST_ASSERT_EQUAL_UINT8(1, r2s_topic_count());
  TEST_ASSERT_EQUAL_STRING("/tf", r2s_topic_get(0)->topic_name);
  TEST_ASSERT_EQUAL_STRING("tf2_msgs/msg/TFMessage", r2s_topic_get(0)->type_name);
  TEST_ASSERT_EQUAL_MEMORY(s_tf_message_hash, r2s_topic_get(0)->type_hash, 8u);
  TEST_ASSERT_EQUAL_UINT8(0x00u, r2s_topic_get(0)->flags & 0x01u);
}

/* The TFMessage payload of one stamped transform, encoded the way the codec
 * does. */
static uint16_t tf_payload(uint8_t *out, const tf::tfMessage & msg, uint8_t narrow)
{
  r2s_codec_out_t sink = {out, R2S_MAX_FRAME, 0u, 0u};
  uint8_t field = 0xFFu;

  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&r2s::r2s_bundled_table_tf2_msgs_TFMessage,
    (uint16_t)R2S_BUNDLED_INDEX_tf2_msgs_TFMessage, &msg,
                                                    narrow, &sink, &field));
  return sink.len;
}

/* sendTransform publishes the one stamped transform on /tf, as in ros_lib:
 * the frame carries the TFMessage the bridge republishes. */
void test_ros_broadcaster_send_transform_publishes_the_frame(void)
{
  ros::NodeHandle nh;
  tf::TransformBroadcaster broadcaster;
  static uint8_t payload[R2S_MAX_FRAME];
  tf::tfMessage expect;
  uint16_t len;

  memset(&expect, 0, sizeof expect);
  expect.transforms.size = 1u;
  expect.transforms.data[0].header.stamp.sec = 7;
  expect.transforms.data[0].header.stamp.nanosec = 8u;
  strcpy(expect.transforms.data[0].header.frame_id, "base");
  strcpy(expect.transforms.data[0].child_frame_id, "gripper");
  expect.transforms.data[0].transform.translation.x = 1.5;
  expect.transforms.data[0].transform.translation.y = -2.0;
  expect.transforms.data[0].transform.translation.z = 0.5;
  expect.transforms.data[0].transform.rotation.x = 0.0;
  expect.transforms.data[0].transform.rotation.y = 0.0;
  expect.transforms.data[0].transform.rotation.z = 0.0;
  expect.transforms.data[0].transform.rotation.w = 1.0;
  nh.initNode();
  broadcaster.init(nh);
  nh.spinOnce();
  handshake(1u);
  fake_port_tx_clear();
  {
    geometry_msgs::TransformStamped transform = expect.transforms.data[0];

    broadcaster.sendTransform(transform);
  }
  len = tf_payload(payload, expect, 0u);
  assert_tx(0, 16u, 0u, payload, len);
}
