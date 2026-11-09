// The router between the MCU's topics and the ROS graph: remapping,
// QoS depth, paced host-to-MCU traffic and converted payloads both ways,
// through a real node with a scripted link and real type support.
// covers: SES-16, SES-18, SES-20, SES-27, SES-31, FRM-11
// covers: LOG-1, LOG-2
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <geometry_msgs/msg/twist.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/int32_multi_array.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_srvs/srv/trigger.hpp>

#include "frame_helpers.hpp"
#include "manual_clock.hpp"
#include "r2s_bridge/router.hpp"
#include "rosidl_runtime_c/type_hash.h"

namespace r2s_bridge
{
namespace test
{
namespace
{

constexpr std::uint8_t kBattery = 16u;
constexpr std::uint8_t kCmdVel = 17u;
constexpr std::uint8_t kEchoVel = 18u;
constexpr std::uint8_t kParamsTopic = 3u;
constexpr std::uint8_t kAckTopic = 6u;

std::array<std::uint8_t, 8> installed_hash(const std::string & type)
{
  const auto library = rclcpp::get_typesupport_library(type, "rosidl_typesupport_cpp");
  const rosidl_message_type_support_t *handle =
    rclcpp::get_message_typesupport_handle(type, "rosidl_typesupport_cpp", *library);
  const rosidl_type_hash_t *hash = handle->get_type_hash_func(handle);
  std::array<std::uint8_t, 8> out{};
  std::copy(hash->value, hash->value + 8u, out.begin());
  return out;
}

std::array<std::uint8_t, 8> installed_service_hash(const std::string & type)
{
  const auto library = rclcpp::get_typesupport_library(type, "rosidl_typesupport_cpp");
  const rosidl_service_type_support_t *handle =
    rclcpp::get_service_typesupport_handle(type, "rosidl_typesupport_cpp", *library);
  const rosidl_type_hash_t *hash = handle->get_type_hash_func(handle);
  std::array<std::uint8_t, 8> out{};
  std::copy(hash->value, hash->value + 8u, out.begin());
  return out;
}

constexpr std::uint8_t kServicesTopic = 7u;

bool logged_has(const std::vector<std::string> & logged, const std::string & part)
{
  return std::any_of(logged.begin(), logged.end(),
           [&part](const std::string & line) {return line.find(part) != std::string::npos;});
}

class RouterFixture
{
public:
  RouterFixture(
    const std::string & name, rclcpp::NodeOptions options, RouterParams params,
    std::uint16_t max_frame = 512u, bool with_mcu_sink = true,
    SessionConfig session_config = SessionConfig(), bool with_node_params = true)
  : max_frame_(max_frame), node_(std::make_shared<rclcpp::Node>(name, "robot1", options))
  {
    LogSink mcu_sink;
    if (with_mcu_sink) {
      mcu_sink = [this](SessionLog level, const std::string & text) {
          mcu_.emplace_back(level, text);
        };
    }
    NodeServiceInterfaces node_services;
    node_services.base = node_->get_node_base_interface();
    node_services.graph = node_->get_node_graph_interface();
    node_services.services = node_->get_node_services_interface();
    router_ = std::make_unique<Router>(
      node_->get_node_topics_interface(),
      [this]() {return std::unique_ptr<Transport>(new HelperLink(transport_));},
      clock_,
      clock_,
      std::move(params),
      [this](SessionLog, const std::string & text) {
        logged_.push_back(text);
      },
      std::move(session_config),
      std::move(mcu_sink),
      with_node_params ? node_->get_node_parameters_interface() : nullptr,
      std::move(node_services));
  }

  void block_link()
  {
    transport_.limit_write(0u);
  }

  void allow_link()
  {
    transport_.limit_write(1000000u);
  }

  void advance(std::uint32_t ms)
  {
    clock_.advance(std::chrono::milliseconds(ms));
    router_->poll();
  }

  void deliver_frame(
    std::uint8_t topic_id, std::uint8_t seq,
    const std::vector<std::uint8_t> & payload)
  {
    std::vector<std::uint8_t> decoded{topic_id, seq};
    decoded.insert(decoded.end(), payload.begin(), payload.end());
    transport_.script_read(helper_wire_of(helper_with_check(decoded)));
    router_->poll();
  }

    // Answers what the session asks until CONNECTED: a Float32 publisher
    // "battery" and a Twist subscription "cmd_vel", with an optional String
    // topic "log_text" in either direction.
  static constexpr std::uint8_t kLogText = 18u;
  void handshake(
    std::uint8_t cmd_vel_flags = 0x01u, std::uint32_t token = 7u, bool with_log_text = false,
    std::uint8_t log_text_flags = 0x00u, std::uint16_t frame_cap = 0u)
  {
    const std::uint16_t cap = frame_cap == 0u ? max_frame_ : frame_cap;
    deliver_frame(kHelperControl, 0u, helper_hello(token, with_log_text ? 3u : 2u, cap));
    expect_topic_req(0u);
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    deliver_frame(
            kHelperControl, 1u,
            helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32",
            "battery"));
    expect_topic_req(1u);
    const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
    deliver_frame(kHelperControl, 2u,
                      helper_topic(1u, kCmdVel, cmd_vel_flags, twist_hash.data(),
            "geometry_msgs/msg/Twist",
                                   "cmd_vel"));
    if (with_log_text) {
      expect_topic_req(2u);
      const std::array<std::uint8_t, 8> string_hash = installed_hash("std_msgs/msg/String");
      deliver_frame(kHelperControl, 3u,
                          helper_topic(2u, kLogText, log_text_flags, string_hash.data(),
                                       "std_msgs/msg/String", "log_text"));
    }
    drain();
    const bool log_is_pub = with_log_text && (log_text_flags & 0x01u) == 0u;
    EXPECT_EQ(router_->publisher_count(), log_is_pub ? 2u : 1u);
    EXPECT_EQ(router_->subscription_count(), with_log_text && !log_is_pub ? 2u : 1u);
  }

  std::vector<std::uint8_t> hello_two()
  {
    return helper_hello(7u, 2u, max_frame_);
  }

    // One drained unit, decoded, with its CRC checked.
  std::vector<std::uint8_t> expect_one_frame()
  {
    const std::vector<std::vector<std::uint8_t>> units = drain();
    EXPECT_EQ(units.size(), 1u);
    if (units.size() != 1u) {
      return {};
    }
    const std::vector<std::uint8_t> decoded = checked_decode(units[0]);
    return decoded;
  }

  std::vector<std::uint8_t> checked_decode(const std::vector<std::uint8_t> & unit)
  {
    const std::vector<std::uint8_t> decoded = helper_decode_unit(unit);
    EXPECT_FALSE(decoded.empty()) << "the capture is not rCOBS";
    if (decoded.size() < 4u) {
      return {};
    }
    const std::uint16_t check = static_cast<std::uint16_t>(r2s_crc16_update_buf(
            R2S_CRC16_INIT, decoded.data(), static_cast<std::uint16_t>(decoded.size() - 2u)));
    EXPECT_EQ(static_cast<std::uint8_t>(check & 0xFFu), decoded[decoded.size() - 2u]);
    EXPECT_EQ(static_cast<std::uint8_t>(check >> 8u), decoded[decoded.size() - 1u]);
    return decoded;
  }

  void expect_topic_req(std::uint8_t index)
  {
        // An answered TOPIC leaves its STATUS next to the following REQ.
    const std::vector<std::vector<std::uint8_t>> units = drain();
    ASSERT_FALSE(units.empty());
    const std::vector<std::uint8_t> decoded = checked_decode(units.back());
    ASSERT_GE(decoded.size(), 4u);
    EXPECT_EQ(decoded[0], kHelperControl);
    EXPECT_EQ(decoded[2], 0x03u) << "a TOPIC_REQ";
    EXPECT_EQ(decoded[3], index);
  }

  void expect_param_req(std::uint8_t index)
  {
    const std::vector<std::vector<std::uint8_t>> units = drain();
    ASSERT_FALSE(units.empty());
    const std::vector<std::uint8_t> decoded = checked_decode(units.back());
    ASSERT_GE(decoded.size(), 4u);
    EXPECT_EQ(decoded[0], 0x03u);
    EXPECT_EQ(decoded[2], 0x01u) << "a PARAM_REQ";
    EXPECT_EQ(decoded[3], index);
  }

    // One decoded PARAM_SET per parameter index among the drained units.
  struct WireSet
  {
    std::uint8_t rel;
    std::vector<std::uint8_t> value;
  };
  std::map<std::uint8_t, WireSet> param_sets()
  {
    std::map<std::uint8_t, WireSet> out;
    for (const auto & unit : drain()) {
      const std::vector<std::uint8_t> decoded = checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kParamsTopic && decoded[2] == 0x03u) {
        out[decoded[4]] =
          WireSet{decoded[3], std::vector<std::uint8_t>(decoded.begin() + 5, decoded.end() - 2)};
      }
    }
    return out;
  }

    // One connected router with the handshake's parameters: led, rate,
    // gain, label and read-only ro with the defaults the tests assert.
    // Without the full table only "led" is announced; with long_label the
    // label default is 200 bytes. Returns the writes the handshake's last
    // step left: READY and whatever the parameters layer pushed, so the
    // caller can inspect them.
  std::vector<std::vector<std::uint8_t>> handshake_with_params(
    std::uint32_t token = 7u, bool full = true,
    bool led_i64 = false,
    bool long_label = false)
  {
    deliver_frame(kHelperControl, 0u, helper_hello(token, 2u, max_frame_, 0x04u));
    expect_topic_req(0u);
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    deliver_frame(
            kHelperControl, 1u,
            helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32",
            "battery"));
    expect_topic_req(1u);
    const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
    deliver_frame(
            kHelperControl, 2u,
            helper_topic(1u, kCmdVel, 0x01u, twist_hash.data(), "geometry_msgs/msg/Twist",
            "cmd_vel"));
    expect_param_req(0u);
    if (led_i64) {
      deliver_frame(kParamsTopic, 0u,
                          helper_declare(0u, 0x01u, 0x00u, "led",
        {0x03u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}));
    } else {
      deliver_frame(kParamsTopic, 0u, helper_declare(0u, 0x00u, 0x00u, "led", {0x01u}));
    }
    expect_param_req(1u);
    if (!full) {
      deliver_frame(kParamsTopic, 1u, {0x04u, 0x00u, 0x01u, 0x01u});
      return drain();
    }
    deliver_frame(kParamsTopic, 1u,
                      helper_declare(1u, 0x01u, 0x00u, "rate",
      {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}));
    expect_param_req(2u);
    deliver_frame(kParamsTopic, 2u,
                      helper_declare(2u, 0x02u, 0x00u, "gain",
      {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xF8u, 0x3Fu}));
    expect_param_req(3u);
    if (long_label) {
      std::vector<std::uint8_t> wide{0xC8u, 0x01u};
      wide.insert(wide.end(), 200u, 'x');
      deliver_frame(kParamsTopic, 3u, helper_declare(3u, 0x03u, 0x00u, "label", wide));
    } else {
      deliver_frame(kParamsTopic, 3u, helper_declare(3u, 0x03u, 0x00u, "label", {0x02u, 'h', 'i'}));
    }
    expect_param_req(4u);
    deliver_frame(
            kParamsTopic, 4u,
            helper_declare(4u, 0x01u, 0x01u, "ro",
      {0x07u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}));
    expect_param_req(5u);
    deliver_frame(kParamsTopic, 5u, helper_declare(5u, 0x00u, 0x00u, "", {0x01u}));
    expect_param_req(6u);
    deliver_frame(kParamsTopic, 6u, {0x04u, 0x00u, 0x06u, 0x01u});
    return drain();
  }

  std::vector<std::vector<std::uint8_t>> drain()
  {
    std::vector<std::uint8_t> all;
    for (const auto & write : transport_.writes()) {
      all.insert(all.end(), write.begin(), write.end());
    }
    transport_.discard_writes();
    return helper_units_of(all);
  }

  Router & router()
  {
    return *router_;
  }
  rclcpp::Node::SharedPtr node()
  {
    return node_;
  }

    // A handshake announcing one served Trigger "reset" and one called
    // Trigger "trigger", with the installed hashes.
  void handshake_with_services()
  {
    deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0x4Cu));
    deliver_frame(kHelperControl, 1u,
      helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
        "std_msgs/msg/Float32", "battery"));
    deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
    const std::array<std::uint8_t, 8> hash = installed_service_hash("std_srvs/srv/Trigger");
    deliver_frame(kServicesTopic, 0u,
      helper_service_declare(0u, 0x00u, hash.data(), "std_srvs/srv/Trigger", "reset"));
    deliver_frame(kServicesTopic, 1u,
      helper_service_declare(1u, 0x01u, hash.data(), "std_srvs/srv/Trigger", "trigger"));
    deliver_frame(kServicesTopic, 2u,
      std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x02u, 0x01u});
  }
  const std::vector<std::string> & logged() const
  {
    return logged_;
  }
  const std::vector<std::pair<SessionLog, std::string>> & mcu_logged() const
  {
    return mcu_;
  }

private:
  std::uint16_t max_frame_;
  FakeTransport transport_;
  ManualClock clock_;
  rclcpp::Node::SharedPtr node_;
  std::unique_ptr<Router> router_;
  std::vector<std::string> logged_;
  std::vector<std::pair<SessionLog, std::string>> mcu_;
};

class RouterNode : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    rclcpp::init(0, nullptr);
  }
  static void TearDownTestSuite()
  {
    rclcpp::shutdown();
  }
};

// A three-topic walk whose echo_vel the middleware refuses: the type
// resolves, so the verdict is ok, but no route serves it.
void handshake_with_uncreatable_echo(RouterFixture & fixture)
{
  const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
  const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(7u, 3u));
  fixture.expect_topic_req(0u);
  fixture.deliver_frame(
        kHelperControl, 1u,
        helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
  fixture.expect_topic_req(1u);
  fixture.deliver_frame(
        kHelperControl, 2u,
        helper_topic(1u, kCmdVel, 0x01u, twist_hash.data(), "geometry_msgs/msg/Twist", "cmd_vel"));
  fixture.expect_topic_req(2u);
  fixture.deliver_frame(
        kHelperControl, 3u,
        helper_topic(2u, kEchoVel, 0x00u, twist_hash.data(), "geometry_msgs/msg/Twist",
          "not a topic name"));
  fixture.drain();
}

rclcpp::NodeOptions remapped()
{
  return rclcpp::NodeOptions().arguments({"--ros-args", "-r", "battery:=power/battery"});
}

std::map<std::string, std::string> topic_map(rclcpp::Node::SharedPtr node)
{
  std::map<std::string, std::string> out;
  for (const auto &[name, types] : node->get_topic_names_and_types()) {
    if (!types.empty()) {
      out[name] = types[0];
    }
  }
  return out;
}

TEST_F(RouterNode, RemappingAppliesToTheMcuTopics)
{
    RouterFixture fixture("remap", remapped(), RouterParams{});
    fixture.handshake();
    const std::map<std::string, std::string> topics = topic_map(fixture.node());
    EXPECT_EQ(topics.count("/robot1/power/battery"), 1u) << "the launch remapping renames battery";
    auto cmd_vel = topics.find("/robot1/cmd_vel");
    ASSERT_NE(cmd_vel, topics.end());
    EXPECT_EQ(cmd_vel->second, "geometry_msgs/msg/Twist");
}

TEST_F(RouterNode, QosDepthSetsHistoryDepth)
{
    RouterParams params;
    params.qos_depth = 7;
    RouterFixture fixture("qos", remapped(), params);
    fixture.handshake(0x09u);
    const std::optional<rclcpp::QoS> pub_qos = fixture.router().endpoint_qos(kBattery);
    ASSERT_TRUE(pub_qos.has_value());
    EXPECT_EQ(pub_qos->history(), rclcpp::HistoryPolicy::KeepLast);
    EXPECT_EQ(pub_qos->depth(), 7u);
    EXPECT_EQ(pub_qos->reliability(), rclcpp::ReliabilityPolicy::BestEffort);
    const std::optional<rclcpp::QoS> sub_qos = fixture.router().endpoint_qos(kCmdVel);
    ASSERT_TRUE(sub_qos.has_value());
    EXPECT_EQ(sub_qos->history(), rclcpp::HistoryPolicy::KeepLast);
    EXPECT_EQ(sub_qos->depth(), 7u) << "the reliable flag selects reliable delivery";
    EXPECT_EQ(sub_qos->reliability(), rclcpp::ReliabilityPolicy::Reliable);
    EXPECT_FALSE(fixture.router().endpoint_qos(18u).has_value()) << "no such topic is routed";
}

TEST_F(RouterNode, McuToHostForwardsTheConvertedMessage)
{
    RouterFixture fixture("up", remapped(), RouterParams{});
    fixture.handshake();
    std::vector<float> received;
    // The bridge publishes best effort on this topic ([SES-31]), so the
    // probe subscribes best effort too; a reliable probe would never match.
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "power/battery", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::Float32 & msg) {received.push_back(msg.data);});
    fixture.deliver_frame(kBattery, 0u, {0x00, 0x00, 0xC0, 0x3F});
    rclcpp::spin_some(fixture.node());
    ASSERT_EQ(received.size(), 1u);
    EXPECT_FLOAT_EQ(received[0], 1.5F);
}

TEST_F(RouterNode, HostToMcuSendsThePackedMessage)
{
    RouterFixture fixture("down", remapped(), RouterParams{});
    fixture.handshake();
    auto pub = fixture.node()->create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
    geometry_msgs::msg::Twist twist;
    twist.linear.x = 1.0;
    pub->publish(twist);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    const std::vector<std::uint8_t> decoded = fixture.expect_one_frame();
    ASSERT_GE(decoded.size(), 4u);
    EXPECT_EQ(decoded[0], kCmdVel);
    EXPECT_EQ(decoded[1], 0u) << "the first frame on a topic carries seq 0";
    std::vector<std::uint8_t> want{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F};
    want.resize(48u, 0x00u);
    EXPECT_EQ(std::vector<std::uint8_t>(decoded.begin() + 2u, decoded.end() - 2u), want);
}

TEST_F(RouterNode, BadPayloadLogsTheTopicAndField)
{
    RouterFixture fixture("bad", remapped(), RouterParams{});
    fixture.handshake();
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "power/battery", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::Float32 & msg) {received.push_back(msg.data);});
    fixture.deliver_frame(kBattery, 0u, {0x00});
    rclcpp::spin_some(fixture.node());
    EXPECT_TRUE(received.empty()) << "a truncated payload is dropped, never published";
    EXPECT_TRUE(logged_has(fixture.logged(), "battery")) << "the drop names the topic";
    EXPECT_TRUE(logged_has(fixture.logged(), "data")) << "the drop names the field path";
}

TEST_F(RouterNode, TrailingBytesLogged)
{
    RouterFixture fixture("trail", remapped(), RouterParams{});
    fixture.handshake();
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "power/battery", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::Float32 & msg) {received.push_back(msg.data);});
    fixture.deliver_frame(kBattery, 0u, {0x00, 0x00, 0xC0, 0x3F, 0xFF});
    rclcpp::spin_some(fixture.node());
    EXPECT_TRUE(received.empty()) << "a payload with trailing bytes is dropped, never published";
    EXPECT_TRUE(logged_has(fixture.logged(), "battery")) << "the drop names the topic";
}

TEST_F(RouterNode, BadVarintLogged)
{
    RouterFixture fixture("varint", remapped(), RouterParams{});
    fixture.handshake(0x01u, 7u, true);
    std::vector<std::string> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::String>(
        "log_text", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::String & msg) {received.push_back(msg.data);});
    fixture.deliver_frame(RouterFixture::kLogText, 0u, {0x80, 0x00});
    rclcpp::spin_some(fixture.node());
    EXPECT_TRUE(received.empty()) << "a malformed length is dropped, never published";
    EXPECT_TRUE(logged_has(fixture.logged(), "log_text")) << "the drop names the topic";
}

TEST_F(RouterNode, NulInStringLoggedWithFieldPath)
{
    RouterFixture fixture("nul", remapped(), RouterParams{});
    fixture.handshake(0x01u, 7u, true);
    std::vector<std::string> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::String>(
        "log_text", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::String & msg) {received.push_back(msg.data);});
    fixture.deliver_frame(RouterFixture::kLogText, 0u, {0x03, 0x61, 0x00, 0x62});
    rclcpp::spin_some(fixture.node());
    EXPECT_TRUE(received.empty()) << "a string with NUL is dropped, never published";
    EXPECT_TRUE(logged_has(fixture.logged(), "log_text")) << "the drop names the topic";
    EXPECT_TRUE(logged_has(fixture.logged(), "data")) << "the drop names the field path";
}

TEST_F(RouterNode, OverlongRosStringDropped)
{
    RouterFixture fixture("long", remapped(), RouterParams{});
    fixture.handshake(0x01u, 7u, true, 0x01u);
    auto pub = fixture.node()->create_publisher<std_msgs::msg::String>("log_text", 10);
    std_msgs::msg::String text;
    text.data = std::string(70000, 'x');
    pub->publish(text);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_TRUE(fixture.drain().empty()) << "an overlong string never goes out";
    EXPECT_TRUE(logged_has(fixture.logged(), "log_text")) << "the drop names the topic";
}

TEST_F(RouterNode, BadNameTopicGetsNoRoute)
{
    RouterFixture fixture("badname", remapped(), RouterParams{});
    fixture.deliver_frame(kHelperControl, 0u, fixture.hello_two());
    fixture.expect_topic_req(0u);
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    fixture.deliver_frame(
        kHelperControl, 1u,
        helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
    fixture.expect_topic_req(1u);
    const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
    fixture.deliver_frame(
        kHelperControl, 2u,
        helper_topic(1u, kCmdVel, 0x01u, twist_hash.data(), "geometry_msgs/msg/Twist",
          "not a topic name"));
    fixture.drain();
    EXPECT_EQ(fixture.router().publisher_count(), 1u);
    EXPECT_EQ(fixture.router().subscription_count(), 0u) << "an uncreatable name gets no route";
    EXPECT_TRUE(logged_has(fixture.logged(), "not a topic name")) << "the refusal names the topic";
}

TEST_F(RouterNode, UnknownTypeTopicGetsNoRoute)
{
    RouterFixture fixture("unknowntype", remapped(), RouterParams{});
    fixture.deliver_frame(kHelperControl, 0u, fixture.hello_two());
    fixture.expect_topic_req(0u);
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    fixture.deliver_frame(
        kHelperControl, 1u,
        helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
    fixture.expect_topic_req(1u);
    const std::array<std::uint8_t, 8> zero{};
    fixture.deliver_frame(kHelperControl, 2u,
                          helper_topic(1u, kCmdVel, 0x01u, zero.data(), "nope/msg/Nope", "nope"));
    fixture.drain();
    EXPECT_EQ(fixture.router().publisher_count(), 1u);
    EXPECT_EQ(fixture.router().subscription_count(), 0u) << "an unknown type gets no route";
    EXPECT_TRUE(logged_has(fixture.logged(), "nope")) << "the refusal names the topic";
}

TEST_F(RouterNode, BadBoolLoggedWithFieldPath)
{
    RouterFixture fixture("badbool", remapped(), RouterParams{});
    fixture.deliver_frame(kHelperControl, 0u, helper_hello(9u, 1u, 512u));
    fixture.expect_topic_req(0u);
    const std::array<std::uint8_t, 8> bool_hash = installed_hash("std_msgs/msg/Bool");
    fixture.deliver_frame(kHelperControl, 1u,
                          helper_topic(0u, kBattery, 0x00u, bool_hash.data(), "std_msgs/msg/Bool",
          "flag"));
    fixture.drain();
    EXPECT_EQ(fixture.router().publisher_count(), 1u);
    std::vector<bool> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Bool>(
        "flag", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::Bool & msg) {received.push_back(msg.data);});
    fixture.deliver_frame(kBattery, 0u, {0x02});
    rclcpp::spin_some(fixture.node());
    EXPECT_TRUE(received.empty()) << "a bad bool byte is dropped, never published";
    EXPECT_TRUE(logged_has(fixture.logged(), "flag")) << "the drop names the topic";
    EXPECT_TRUE(logged_has(fixture.logged(), "data")) << "the drop names the field path";
}

TEST_F(RouterNode, FramesOutsideARouteAreIgnored)
{
    RouterFixture fixture("stray", remapped(), RouterParams{});
    fixture.handshake();
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "power/battery", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::Float32 & msg) {received.push_back(msg.data);});
    fixture.deliver_frame(20u, 0u, {0x00, 0x00, 0xC0, 0x3F});
    fixture.deliver_frame(kCmdVel, 0u, {0x01});
    rclcpp::spin_some(fixture.node());
    EXPECT_TRUE(received.empty()) << "no route means no publication";
}

TEST_F(RouterNode, KeepLatestSubDropsTheOlderMessage)
{
    RouterFixture fixture("latest", remapped(), RouterParams{});
    fixture.handshake(0x05u);
    auto pub = fixture.node()->create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
    geometry_msgs::msg::Twist first;
    first.linear.x = 1.0;
    pub->publish(first);
    rclcpp::spin_some(fixture.node());
    geometry_msgs::msg::Twist second;
    second.linear.x = 2.0;
    pub->publish(second);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_EQ(fixture.router().dropped(kCmdVel), 1u) << "the replaced message is counted";
    const std::vector<std::uint8_t> decoded = fixture.expect_one_frame();
    ASSERT_GE(decoded.size(), 12u);
    const std::vector<std::uint8_t> want{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40};
    EXPECT_EQ(std::vector<std::uint8_t>(decoded.begin() + 2u, decoded.begin() + 10u), want)
        << "only the newest message goes out";
}

TEST_F(RouterNode, OversizeRosMessageIsDropped)
{
    RouterFixture fixture("big", remapped(), RouterParams{}, 20u);
    fixture.handshake();
    auto pub = fixture.node()->create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
    geometry_msgs::msg::Twist twist;
    pub->publish(twist);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_TRUE(fixture.drain().empty()) << "a frame past max_frame never goes out";
    EXPECT_TRUE(logged_has(fixture.logged(), "cmd_vel")) << "the drop names the topic";
    EXPECT_EQ(fixture.router().dropped(kCmdVel), 0u) << "the pacer never saw it";
}

TEST_F(RouterNode, IdenticalRehandshakeKeepsRoutes)
{
    RouterFixture fixture("re", remapped(), RouterParams{});
    fixture.handshake();
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "power/battery", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::Float32 & msg) {received.push_back(msg.data);});
    fixture.handshake(0x01u, 8u);
    EXPECT_EQ(fixture.router().publisher_count(), 1u);
    EXPECT_EQ(fixture.router().subscription_count(), 1u);
    fixture.deliver_frame(kBattery, 0u, {0x00, 0x00, 0xC0, 0x3F});
    rclcpp::spin_some(fixture.node());
    ASSERT_EQ(received.size(), 1u) << "traffic flows again after the re-handshake";
    EXPECT_FLOAT_EQ(received[0], 1.5F);
}

TEST_F(RouterNode, RateOverrideKeysOnTheAnnouncedName)
{
    RouterParams params;
    params.rate_limits["cmd_vel"] = 1.0;
    rclcpp::NodeOptions options;
    options.arguments({"--ros-args", "-r", "cmd_vel:=steer/cmd_vel"});
    RouterFixture fixture("remaprate", options, params);
    fixture.handshake();
    EXPECT_EQ(topic_map(fixture.node()).count("/robot1/steer/cmd_vel"), 1u)
        << "the launch remapping renames the subscription";
    auto pub = fixture.node()->create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
    geometry_msgs::msg::Twist twist;
    pub->publish(twist);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_EQ(fixture.drain().size(), 1u) << "the first frame goes out at once";
    pub->publish(twist);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_TRUE(fixture.drain().empty()) << "the announced-name override paces the remapped topic";
    fixture.advance(1000u);
    const std::vector<std::vector<std::uint8_t>> last = fixture.drain();
    ASSERT_EQ(last.size(), 2u) << "the heartbeat and the paced frame";
    const std::vector<std::uint8_t> heartbeat = fixture.checked_decode(last[0]);
    ASSERT_GE(heartbeat.size(), 1u);
    EXPECT_EQ(heartbeat[0], 4u) << "the heartbeat goes out on its period";
    const std::vector<std::uint8_t> paced = fixture.checked_decode(last[1]);
    ASSERT_GE(paced.size(), 4u);
    EXPECT_EQ(paced[0], kCmdVel) << "the second frame follows its second";
}

TEST_F(RouterNode, RehandshakeRefreshesTheBurst)
{
    RouterParams params;
    params.tx_budget_pct = 10;
    RouterFixture fixture("burst", remapped(), params);
    fixture.handshake(0x01u, 7u, true, 0x01u, 512u);
    auto pub = fixture.node()->create_publisher<std_msgs::msg::String>("log_text", 10);
    std_msgs::msg::String text;
    text.data = std::string(1500, 'z');
    const auto log_frames = [&fixture]() {
      std::size_t count = 0;
      for (const auto & unit : fixture.drain()) {
        const std::vector<std::uint8_t> decoded = fixture.checked_decode(unit);
        if (!decoded.empty() && decoded[0] == RouterFixture::kLogText) {
          ++count;
        }
      }
      return count;
    };
    pub->publish(text);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_EQ(log_frames(), 0u) << "a 1506 byte frame never fits a 512 byte max_frame";
    fixture.handshake(0x01u, 8u, true, 0x01u, 2048u);
    EXPECT_EQ(fixture.router().subscription_count(), 2u) << "the identical table keeps its routes";
    pub->publish(text);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_EQ(log_frames(), 0u) << "the 1152 byte budget cannot carry 1513 bytes yet";
    fixture.advance(1000u);
    EXPECT_EQ(log_frames(), 1u) << "the refreshed burst carries the frame";
}

TEST_F(RouterNode, MissingRouteIsRetriedOnLaterPolls)
{
    RouterFixture fixture("repair", remapped(), RouterParams{});
    // The verdict checks the type, never the name, so an uncreatable name
    // still resolves: the handshake reports it ok while no route serves it.
    handshake_with_uncreatable_echo(fixture);
    EXPECT_EQ(fixture.router().publisher_count(), 1u);
    EXPECT_EQ(fixture.router().subscription_count(), 1u);
    // Later polls retry the missing route instead of losing it silently: a
    // poll inside the spacing skips, and attempts stop after the episode's
    // cap until the next handshake renews them.
    fixture.advance(150u);
    fixture.advance(50u);
    fixture.advance(150u);
    fixture.advance(150u);
    for (int i = 0; i < 8; ++i) {
    fixture.advance(150u);
    }
    const std::size_t echo_lines =
    std::count_if(fixture.logged().begin(), fixture.logged().end(), [](const std::string & line) {
        return line.find("not a topic name") != std::string::npos;
        });
    EXPECT_GE(echo_lines, 2u) << "the missing route is retried on later polls";
    EXPECT_EQ(fixture.router().publisher_count(),
          1u) << "an uncreatable name still serves no route";
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "power/battery", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::Float32 & msg) {received.push_back(msg.data);});
    fixture.deliver_frame(kBattery, 0u, {0x00, 0x00, 0xC0, 0x3F});
    rclcpp::spin_some(fixture.node());
    ASSERT_EQ(received.size(), 1u) << "the healthy route keeps flowing";
}

TEST_F(RouterNode, TwoMissingRoutesAreRetried)
{
    RouterFixture fixture("repairtwo", remapped(), RouterParams{});
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
    fixture.deliver_frame(kHelperControl, 0u, helper_hello(7u, 3u));
    fixture.expect_topic_req(0u);
    fixture.deliver_frame(
        kHelperControl, 1u,
        helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
    fixture.expect_topic_req(1u);
    fixture.deliver_frame(
        kHelperControl, 2u,
        helper_topic(1u, kCmdVel, 0x01u, twist_hash.data(), "geometry_msgs/msg/Twist",
          "not a topic name"));
    fixture.expect_topic_req(2u);
    fixture.deliver_frame(
        kHelperControl, 3u,
        helper_topic(2u, kEchoVel, 0x00u, twist_hash.data(), "geometry_msgs/msg/Twist",
          "bad topic name!"));
    fixture.drain();
    EXPECT_EQ(fixture.router().publisher_count(), 1u);
    EXPECT_EQ(fixture.router().subscription_count(), 0u);
    fixture.advance(150u);
    fixture.advance(150u);
    fixture.advance(150u);
    const auto count_named = [&fixture](const std::string & name) {
      return std::count_if(
            fixture.logged().begin(), fixture.logged().end(),
        [&name](const std::string & line) {
          return line.find("'" + name + "'") != std::string::npos;
            });
    };
    EXPECT_GE(count_named("not a topic name"), 2u) << "the missing subscription is retried";
    EXPECT_GE(count_named("bad topic name!"), 2u) << "the missing publisher is retried";
}

TEST_F(RouterNode, MissingRouteSurfacesInDiagnostics)
{
    RouterFixture fixture("diagmissing", remapped(), RouterParams{});
    handshake_with_uncreatable_echo(fixture);
    const Router::Diagnostics diagnostics = fixture.router().diagnostics();
    ASSERT_EQ(diagnostics.topics.size(), 3u);
    EXPECT_FALSE(diagnostics.topics[0].route_missing) << "battery keeps its route";
    EXPECT_FALSE(diagnostics.topics[1].route_missing) << "cmd_vel keeps its route";
    EXPECT_TRUE(diagnostics.topics[2].route_missing) << "the uncreatable route reports missing";
    EXPECT_FALSE(diagnostics.topics[2].route_error.empty()) << "the missing route names its reason";
}

// covers: PAR-9
TEST_F(RouterNode, McuParametersBecomeRosParameters)
{
    RouterFixture fixture("params", remapped(), RouterParams{});
    fixture.handshake_with_params();
    ASSERT_TRUE(fixture.node()->has_parameter("led")) << "the MCU bool is a ROS parameter";
    EXPECT_EQ(fixture.node()->get_parameter("led").as_bool(), true);
    ASSERT_TRUE(fixture.node()->has_parameter("rate")) << "the MCU i64 is a ROS parameter";
    EXPECT_EQ(fixture.node()->get_parameter("rate").as_int(), 2);
}

// covers: PAR-1, PAR-8
TEST_F(RouterNode, RosParameterSetSendsASetToTheMcu)
{
    RouterFixture fixture("paramset", remapped(), RouterParams{});
    fixture.handshake_with_params();
    const auto result =
    fixture.node()->set_parameters({rclcpp::Parameter("led", false), rclcpp::Parameter("rate", 5)});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_TRUE(result[0].successful) << result[0].reason;
    EXPECT_TRUE(result[1].successful) << result[1].reason;
    // The first SET goes out at once; the second waits behind it for its
    // ack, as the reliable channel allows one outstanding frame ([REL-2]).
    std::uint8_t led_rel = 0u;
    bool saw_led = false;
    for (const auto & unit : fixture.drain()) {
    const std::vector<std::uint8_t> decoded = fixture.checked_decode(unit);
    if (decoded.size() >= 6u && decoded[0] == kParamsTopic && decoded[2] == 0x03u &&
      decoded[4] == 0x00u)
    {
      EXPECT_EQ(std::vector<std::uint8_t>(decoded.begin() + 5, decoded.end() - 2),
                      std::vector<std::uint8_t>{0x00u});
      led_rel = decoded[3];
      saw_led = true;
    }
    }
    EXPECT_TRUE(saw_led) << "the bool set reaches the wire as one byte";
    fixture.deliver_frame(kAckTopic, 0u, {kParamsTopic, led_rel});
    fixture.advance(1u);
    bool saw_rate = false;
    for (const auto & unit : fixture.drain()) {
    const std::vector<std::uint8_t> decoded = fixture.checked_decode(unit);
    if (decoded.size() >= 6u && decoded[0] == kParamsTopic && decoded[2] == 0x03u &&
      decoded[4] == 0x01u)
    {
      EXPECT_EQ(std::vector<std::uint8_t>(decoded.begin() + 5, decoded.end() - 2),
                      std::vector<std::uint8_t>({0x05u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
          0x00u}));
      saw_rate = true;
    }
    }
    EXPECT_TRUE(saw_rate) << "the i64 set reaches the wire as eight bytes";
}

// covers: PAR-1, PAR-8
TEST_F(RouterNode, LongStringSetUsesMultiByteVarint)
{
    // A 200 byte string needs a two byte length prefix ([ENC-7]): the SET
    // carries the continuation bit the short defaults never exercise.
    RouterFixture fixture("longstring", remapped(), RouterParams{});
    fixture.handshake_with_params();
    const std::string text(200u, 'x');
    const auto result = fixture.node()->set_parameters({rclcpp::Parameter("label", text)});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result[0].successful) << result[0].reason;
    const auto sets = fixture.param_sets();
    ASSERT_EQ(sets.count(3u), 1u);
    std::vector<std::uint8_t> want{0xC8u, 0x01u};
    want.insert(want.end(), text.begin(), text.end());
    EXPECT_EQ(sets.at(3u).value, want);
}

// covers: PAR-1, PAR-3
TEST_F(RouterNode, LongDefaultDecodesMultibyteVarint)
{
    // A 200 byte default exercises the two byte length prefix on the
    // declare path, where the short defaults never loop ([ENC-7]).
    RouterFixture fixture("longdefault", remapped(), RouterParams{});
    const auto writes = fixture.handshake_with_params(7u, true, false, true);
    ASSERT_TRUE(fixture.node()->has_parameter("label"));
    EXPECT_EQ(fixture.node()->get_parameter("label").as_string(), std::string(200u, 'x'));
    for (const auto & unit : writes) {
    const std::vector<std::uint8_t> decoded = fixture.checked_decode(unit);
    EXPECT_FALSE(decoded.size() >= 6u && decoded[0] == kParamsTopic && decoded[2] == 0x03u)
            << "the matching default pushes nothing";
    }
}

// covers: PAR-9
TEST_F(RouterNode, LaunchOverrideIsPushedAfterTheHandshake)
{
    // A launch file sets the MCU value before the handshake: the layer
    // declares the parameter with that value and pushes it, since it
    // differs from the announced default.
    rclcpp::NodeOptions over;
    over.arguments({"--ros-args", "-r", "battery:=power/battery", "-p", "led:=false"});
    RouterFixture fixture("override", over, RouterParams{});
    bool pushed = false;
    for (const auto & unit : fixture.handshake_with_params()) {
    const std::vector<std::uint8_t> decoded = fixture.checked_decode(unit);
    if (decoded.size() >= 6u && decoded[0] == kParamsTopic && decoded[2] == 0x03u &&
      decoded[4] == 0x00u)
    {
      EXPECT_EQ(std::vector<std::uint8_t>(decoded.begin() + 5, decoded.end() - 2),
                      std::vector<std::uint8_t>{0x00u});
      pushed = true;
    }
    }
    ASSERT_TRUE(fixture.node()->has_parameter("led"));
    EXPECT_EQ(fixture.node()->get_parameter("led").as_bool(), false);
    EXPECT_TRUE(pushed) << "the overridden value goes out with SET after READY";
}

// covers: PAR-1, PAR-3, PAR-5
TEST_F(RouterNode, McuParameterCodecsCoverAllTypes)
{
    RouterFixture fixture("codecs", remapped(), RouterParams{});
    fixture.handshake_with_params();
    ASSERT_TRUE(fixture.node()->has_parameter("gain"));
    EXPECT_DOUBLE_EQ(fixture.node()->get_parameter("gain").as_double(), 1.5);
    ASSERT_TRUE(fixture.node()->has_parameter("label"));
    EXPECT_EQ(fixture.node()->get_parameter("label").as_string(), "hi");
    ASSERT_TRUE(fixture.node()->has_parameter("ro"));
    EXPECT_EQ(fixture.node()->get_parameter("ro").as_int(), 7);
    const auto result = fixture.node()->set_parameters(
    {rclcpp::Parameter("gain", 2.5), rclcpp::Parameter("label", std::string("yo"))});
    ASSERT_EQ(result.size(), 2u);
    EXPECT_TRUE(result[0].successful) << result[0].reason;
    EXPECT_TRUE(result[1].successful) << result[1].reason;
    const auto first = fixture.param_sets();
    ASSERT_EQ(first.count(2u), 1u);
    EXPECT_EQ(first.at(2u).value,
              std::vector<std::uint8_t>({0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u, 0x40u}));
    fixture.deliver_frame(kAckTopic, 0u, {kParamsTopic, first.at(2u).rel});
    fixture.advance(1u);
    const auto second = fixture.param_sets();
    ASSERT_EQ(second.count(3u), 1u);
    EXPECT_EQ(second.at(3u).value, std::vector<std::uint8_t>({0x02u, 'y', 'o'}));
    const auto refused = fixture.node()->set_parameters({rclcpp::Parameter("ro", 8)});
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_FALSE(refused[0].successful);
    EXPECT_NE(refused[0].reason.find("read-only"), std::string::npos);
    fixture.deliver_frame(kAckTopic, 1u, {kParamsTopic, second.at(3u).rel});
    fixture.advance(1u);
    const auto cleared = fixture.node()->set_parameters({rclcpp::Parameter("label",
            std::string(""))});
    ASSERT_EQ(cleared.size(), 1u);
    ASSERT_TRUE(cleared[0].successful) << cleared[0].reason;
    const auto third = fixture.param_sets();
    ASSERT_EQ(third.count(3u), 1u);
    EXPECT_EQ(third.at(3u).value,
          std::vector<std::uint8_t>{0x00u}) << "an empty string is one zero byte";
}

// covers: PAR-9
TEST_F(RouterNode, RetiredParametersRejectSets)
{
    // A standard node cannot undeclare: the dropped ROS parameter stays,
    // but its sets fail until the MCU announces it again.
    const auto warnings = [](const RouterFixture & fixture) {
      int found = 0;
      for (const auto & line : fixture.logged()) {
        if (line.find("no longer announced") != std::string::npos) {
          ++found;
        }
      }
      return found;
    };
    RouterFixture fixture("drop", remapped(), RouterParams{});
    fixture.handshake_with_params();
    ASSERT_TRUE(fixture.node()->has_parameter("rate"));
    fixture.handshake_with_params(8u, false);
    EXPECT_TRUE(fixture.node()->has_parameter("led"));
    EXPECT_TRUE(fixture.node()->has_parameter("rate")) << "the ROS parameter stays";
    const int retired = warnings(fixture);
    EXPECT_EQ(retired, 4) << "rate, gain, label and ro retire together";
    fixture.handshake_with_params(9u, false);
    EXPECT_EQ(warnings(fixture), retired) << "an already retired table warns once";
    const auto result = fixture.node()->set_parameters({rclcpp::Parameter("rate", 3)});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_FALSE(result[0].successful);
    EXPECT_NE(result[0].reason.find("no longer announced"), std::string::npos);
    const auto live = fixture.node()->set_parameters({rclcpp::Parameter("led", false)});
    ASSERT_EQ(live.size(), 1u);
    EXPECT_TRUE(live[0].successful) << live[0].reason;
}

// covers: PAR-8
TEST_F(RouterNode, SetsFailWhileDisconnected)
{
    RouterFixture fixture("down", remapped(), RouterParams{});
    fixture.handshake_with_params();
    fixture.advance(3000u);
    const auto result = fixture.node()->set_parameters({rclcpp::Parameter("led", false)});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_FALSE(result[0].successful);
    EXPECT_NE(result[0].reason.find("link is down"), std::string::npos);
}

// covers: PAR-5
TEST_F(RouterNode, RefusedSetIsLogged)
{
    RouterFixture fixture("refused", remapped(), RouterParams{});
    fixture.handshake_with_params();
    const auto applied = fixture.node()->set_parameters({rclcpp::Parameter("led", false)});
    ASSERT_EQ(applied.size(), 1u);
    ASSERT_TRUE(applied[0].successful) << applied[0].reason;
    const auto first = fixture.param_sets();
    ASSERT_EQ(first.count(0u), 1u);
    fixture.deliver_frame(kParamsTopic, 0u, {0x04u, first.at(0u).rel, 0x00u, 0x00u});
    EXPECT_FALSE(logged_has(fixture.logged(), "refused")) << "an applied SET stays quiet";
    fixture.deliver_frame(kAckTopic, 0u, {kParamsTopic, first.at(0u).rel});
    fixture.advance(1u);
    const auto refused = fixture.node()->set_parameters({rclcpp::Parameter("led", true)});
    ASSERT_EQ(refused.size(), 1u);
    ASSERT_TRUE(refused[0].successful) << refused[0].reason;
    const auto second = fixture.param_sets();
    ASSERT_EQ(second.count(0u), 1u);
    fixture.deliver_frame(kParamsTopic, 1u, {0x04u, second.at(0u).rel, 0x00u, 0x02u});
    EXPECT_TRUE(logged_has(fixture.logged(), "refused")) << "the MCU verdict is logged";
}

// covers: PAR-5
TEST_F(RouterNode, RefusedStatusNamesEachReason)
{
    // Every refusal code the session passes on names its reason: one
    // STATUS per parameter, each acked in turn by the reliable layer.
    RouterFixture fixture("reasons", remapped(), RouterParams{});
    fixture.handshake_with_params();
    fixture.deliver_frame(kParamsTopic, 0u, {0x04u, 0x00u, 0x00u, 0x01u});
    fixture.deliver_frame(kParamsTopic, 1u, {0x04u, 0x01u, 0x01u, 0x03u});
    fixture.deliver_frame(kParamsTopic, 2u, {0x04u, 0x02u, 0x02u, 0x04u});
    fixture.deliver_frame(kParamsTopic, 3u, {0x04u, 0x03u, 0x09u, 0x02u});
    EXPECT_TRUE(logged_has(fixture.logged(),
          "parameter #9: the MCU refused the set (bad type or length)"));
    EXPECT_TRUE(logged_has(fixture.logged(),
          "parameter 'led': the MCU refused the set (unknown index)"));
    EXPECT_TRUE(logged_has(fixture.logged(),
          "parameter 'rate': the MCU refused the set (read-only)"));
    EXPECT_TRUE(logged_has(fixture.logged(),
          "parameter 'gain': the MCU refused the set (out of range)"));
}

// covers: PAR-9
TEST_F(RouterNode, NoParametersInterfaceSkipsTheLayer)
{
    // The constructor default keeps the layer off: the walk still
    // completes, but nothing is declared.
    RouterFixture fixture("noparams", remapped(), RouterParams{}, 512u, true, SessionConfig{},
    false);
    fixture.handshake_with_params();
    EXPECT_FALSE(fixture.node()->has_parameter("led"));
}

// covers: PAR-9
TEST_F(RouterNode, MismatchedOverrideIsNotDeclared)
{
    // A launch value of the wrong type cannot become the parameter: the
    // declare refuses it, while the walk still completes.
    rclcpp::NodeOptions over;
    over.arguments({"--ros-args", "-p", "led:=oops"});
    RouterFixture fixture("mistype", over, RouterParams{});
    fixture.handshake_with_params();
    EXPECT_FALSE(fixture.node()->has_parameter("led"));
    EXPECT_TRUE(logged_has(fixture.logged(), "not declared"));
    EXPECT_TRUE(fixture.node()->has_parameter("rate")) << "the table around the gap still declares";
}

// covers: PAR-9
TEST_F(RouterNode, ChangedTypeIsNotPushed)
{
    // A reflash changes led to i64 while the ROS parameter stays a bool:
    // no bool bytes go out as an i64.
    RouterFixture fixture("retype", remapped(), RouterParams{});
    fixture.handshake_with_params();
    ASSERT_TRUE(fixture.node()->has_parameter("led"));
    fixture.handshake_with_params(8u, true, true);
    EXPECT_TRUE(logged_has(fixture.logged(), "type changed"));
    EXPECT_TRUE(fixture.param_sets().empty());
}

// covers: PAR-9
TEST_F(RouterNode, NamelessDeclareIsSkipped)
{
    RouterFixture fixture("noname", remapped(), RouterParams{});
    fixture.handshake_with_params();
    EXPECT_TRUE(logged_has(fixture.logged(), "no name"));
    EXPECT_TRUE(fixture.node()->has_parameter("led")) << "the table around the gap still declares";
    EXPECT_TRUE(fixture.node()->has_parameter("ro"));
}

// covers: PAR-9
TEST_F(RouterNode, CollidingNodeParameterIsNotTracked)
{
    RouterFixture fixture("collide", remapped(), RouterParams{});
    fixture.node()->declare_parameter("led", true);
    fixture.handshake_with_params();
    const auto result = fixture.node()->set_parameters({rclcpp::Parameter("led", false)});
    ASSERT_EQ(result.size(), 1u);
    EXPECT_TRUE(result[0].successful) << result[0].reason;
    EXPECT_TRUE(fixture.param_sets().empty()) << "no SET goes out for a foreign parameter";
    EXPECT_TRUE(logged_has(fixture.logged(), "not tracked"));
}

// covers: PAR-9
TEST_F(RouterNode, RehandshakeRepushesTheCurrentValues)
{
    RouterFixture fixture("paramre", remapped(), RouterParams{});
    fixture.handshake_with_params();
    const auto result = fixture.node()->set_parameters({rclcpp::Parameter("led", false)});
    ASSERT_EQ(result.size(), 1u);
    ASSERT_TRUE(result[0].successful) << result[0].reason;
    fixture.drain();
    // The MCU reboots and announces the same table: the bridge walks it
    // again and pushes the current values back, in the writes the second
    // handshake leaves behind.
    bool repushed = false;
    for (const auto & unit : fixture.handshake_with_params(8u)) {
    const std::vector<std::uint8_t> decoded = fixture.checked_decode(unit);
    if (decoded.size() >= 6u && decoded[0] == kParamsTopic && decoded[2] == 0x03u &&
      decoded[4] == 0x00u)
    {
      EXPECT_EQ(std::vector<std::uint8_t>(decoded.begin() + 5, decoded.end() - 2),
                      std::vector<std::uint8_t>{0x00u});
      repushed = true;
    }
    }
    EXPECT_TRUE(repushed) << "the current value goes out again after the re-handshake";
}

TEST_F(RouterNode, RejectsNonsenseParams)
{
    RouterParams no_depth;
    no_depth.qos_depth = 0;
    EXPECT_THROW(RouterFixture("p1", remapped(), no_depth), std::invalid_argument);
    RouterParams no_budget;
    no_budget.tx_budget_pct = 101;
    EXPECT_THROW(RouterFixture("p2", remapped(), no_budget), std::invalid_argument);
    RouterParams no_baud;
    no_baud.baud = 0;
    EXPECT_THROW(RouterFixture("p3", remapped(), no_baud), std::invalid_argument);
    RouterParams no_default;
    no_default.rate_default_hz = -1.0;
    EXPECT_THROW(RouterFixture("p4", remapped(), no_default), std::invalid_argument);
    RouterParams no_override;
    no_override.rate_limits["cmd_vel"] = -2.0;
    EXPECT_THROW(RouterFixture("p5", remapped(), no_override), std::invalid_argument);
    RouterParams negative_budget;
    negative_budget.tx_budget_pct = -1;
    EXPECT_THROW(RouterFixture("p6", remapped(), negative_budget), std::invalid_argument);
    RouterParams infinite_default;
    infinite_default.rate_default_hz = std::numeric_limits<double>::infinity();
    EXPECT_THROW(RouterFixture("p7", remapped(), infinite_default), std::invalid_argument);
    RouterParams infinite_override;
    infinite_override.rate_limits["cmd_vel"] = std::numeric_limits<double>::infinity();
    EXPECT_THROW(RouterFixture("p8", remapped(), infinite_override), std::invalid_argument);
    SessionConfig bad_session;
    bad_session.reconnect_period_ms = 0;
    EXPECT_THROW(RouterFixture("p9", remapped(), RouterParams{}, 512u, true, bad_session),
                 std::invalid_argument);
}

TEST_F(RouterNode, RejectsAnEmptySink)
{
    ManualClock clock;
    FakeTransport transport;
    auto node = std::make_shared<rclcpp::Node>("sink", "robot1");
    EXPECT_THROW(Router(
                     node->get_node_topics_interface(),
      [&transport]() {return std::unique_ptr<Transport>(new HelperLink(transport));}, clock,
                     clock, RouterParams{}, LogSink{}),
                 std::invalid_argument);
}

// One log payload ([LOG-1]): the level, the text as a string, then the value
// kind with no value.
std::vector<std::uint8_t> mcu_log_payload(std::uint8_t level, const std::string & text)
{
  std::vector<std::uint8_t> payload{level, static_cast<std::uint8_t>(text.size())};
  payload.insert(payload.end(), text.begin(), text.end());
  payload.push_back(0u);
  return payload;
}

TEST_F(RouterNode, ForwardsMcuLogLinesToTheirOwnSink)
{
    RouterFixture fixture("mcu_log", remapped(), RouterParams{});
    fixture.handshake();
    fixture.deliver_frame(2u, 0u, mcu_log_payload(30u, "hot"));
    fixture.deliver_frame(2u, 1u, mcu_log_payload(40u, "hotter"));
    ASSERT_EQ(fixture.mcu_logged().size(), 2u);
    EXPECT_EQ(fixture.mcu_logged()[0].first, SessionLog::kWarning);
    EXPECT_EQ(fixture.mcu_logged()[0].second, "hot");
    EXPECT_EQ(fixture.mcu_logged()[1].first, SessionLog::kError);
    EXPECT_EQ(fixture.mcu_logged()[1].second, "hotter");
    EXPECT_TRUE(fixture.logged().empty()) << "MCU lines bypass the bridge sink";
}

TEST_F(RouterNode, FallsBackToTheLogSinkWithoutAnMcuSink)
{
    RouterFixture fixture("mcu_fallback", remapped(), RouterParams{}, 512u, false);
    fixture.handshake();
    fixture.deliver_frame(2u, 0u, mcu_log_payload(20u, "line"));
    EXPECT_TRUE(fixture.mcu_logged().empty());
    ASSERT_EQ(fixture.logged().size(), 1u);
    EXPECT_EQ(fixture.logged()[0], "line");
}

TEST(RouterOverrides, ParsesTopicRates)
{
    std::vector<std::string> logged;
    const std::map<std::string, double> rates =
    parse_rate_overrides({"cmd_vel=20", "battery=0.5"},
      [&logged](SessionLog, const std::string & text) {logged.push_back(text);});
    EXPECT_EQ(rates.size(), 2u);
    EXPECT_DOUBLE_EQ(rates.at("cmd_vel"), 20.0);
    EXPECT_DOUBLE_EQ(rates.at("battery"), 0.5);
    EXPECT_TRUE(logged.empty());
}

TEST(RouterOverrides, SkipsMalformedEntries)
{
    std::vector<std::string> logged;
    const std::map<std::string, double> rates =
    parse_rate_overrides({"bogus", "cmd_vel=-1", "again=1=2", "x==", "=5", "topic=", "fast=inf"},
      [&logged](SessionLog, const std::string & text) {logged.push_back(text);});
    EXPECT_TRUE(rates.empty());
    EXPECT_TRUE(logged_has(logged, "bogus"));
    EXPECT_TRUE(logged_has(logged, "cmd_vel=-1"));
    EXPECT_TRUE(logged_has(logged, "x=="));
    EXPECT_TRUE(logged_has(logged, "=5"));
    EXPECT_TRUE(logged_has(logged, "topic="));
    EXPECT_TRUE(logged_has(logged, "fast=inf"));
}

TEST(RouterOverrides, IgnoresOverridesWithoutASink)
{
    const std::map<std::string, double> rates = parse_rate_overrides({"cmd_vel=20"}, nullptr);
    ASSERT_EQ(rates.size(), 1u);
    EXPECT_DOUBLE_EQ(rates.at("cmd_vel"), 20.0);
}

TEST(RouterOverrides, DropsAGarbageOverrideWithoutASink)
{
    const std::map<std::string, double> rates = parse_rate_overrides({"cmd_vel="}, nullptr);
    EXPECT_TRUE(rates.empty()) << "a garbage override is dropped even when no sink logs it";
}

TEST_F(RouterNode, MalformedLengthPrefixIsABadVarint)
{
    // A string length past the 16-bit wire limit is malformed, never a huge
    // allocation: the drop names the varint instead of going out. The frame
    // stays small, so the framer passes it to conversion.
    RouterFixture fixture("badlength", remapped(), RouterParams{});
    fixture.handshake(0x01u, 7u, true, 0x08u);
    std::vector<std::uint8_t> payload{0xF0u, 0xA2u, 0x04u};
    payload.insert(payload.end(), 10u, 0x78u);
    fixture.deliver_frame(RouterFixture::kLogText, 0u, payload);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_TRUE(logged_has(fixture.logged(), "bad varint")) << "the drop names the varint";
    EXPECT_TRUE(logged_has(fixture.logged(), "log_text")) << "the drop names the topic";
}

TEST_F(RouterNode, OverlongArrayFromRosNamesOverBound)
{
    // A ROS sequence past the 64 KiB wire limit cannot pack, so the drop
    // names the bound instead of queueing.
    constexpr std::uint8_t kBig = 19u;
    RouterFixture fixture("bigarray", remapped(), RouterParams{});
    fixture.deliver_frame(kHelperControl, 0u, helper_hello(9u, 2u, 512u));
    fixture.expect_topic_req(0u);
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    fixture.deliver_frame(
        kHelperControl, 1u,
        helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
    fixture.expect_topic_req(1u);
    const std::array<std::uint8_t, 8> array_hash = installed_hash("std_msgs/msg/Int32MultiArray");
    fixture.deliver_frame(
        kHelperControl, 2u,
        helper_topic(1u, kBig, 0x09u, array_hash.data(), "std_msgs/msg/Int32MultiArray", "big"));
    fixture.drain();
    ASSERT_EQ(fixture.router().subscription_count(), 1u) << "the array topic is subscribed";
    auto pub = fixture.node()->create_publisher<std_msgs::msg::Int32MultiArray>("big", 10);
    std_msgs::msg::Int32MultiArray array;
    array.data.assign(70000u, 1);
    pub->publish(array);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_TRUE(logged_has(fixture.logged(), "over bound")) << "the drop names the bound";
    EXPECT_TRUE(logged_has(fixture.logged(), "big")) << "the drop names the topic";
}

TEST_F(RouterNode, IdenticalEmptyHandshakeRebuilds)
{
    RouterFixture fixture("empty", remapped(), RouterParams{});
    fixture.deliver_frame(kHelperControl, 0u, helper_hello(7u, 0u, 512u));
    EXPECT_EQ(fixture.router().publisher_count(), 0u);
    EXPECT_EQ(fixture.router().subscription_count(), 0u);
    std::vector<std::uint8_t> ready = fixture.expect_one_frame();
    ASSERT_GE(ready.size(), 4u);
    EXPECT_EQ(ready[2], 0x06u) << "a READY";
    fixture.deliver_frame(kHelperControl, 1u, helper_hello(7u, 0u, 512u));
    EXPECT_EQ(fixture.router().publisher_count(), 0u);
    EXPECT_EQ(fixture.router().subscription_count(), 0u);
    ready = fixture.expect_one_frame();
    ASSERT_GE(ready.size(), 4u);
    EXPECT_EQ(ready[2], 0x06u) << "a second READY, not an early return";
}

TEST_F(RouterNode, DropsMcuFramesOnSubscriptionTopics)
{
    RouterFixture fixture("wrongdir", remapped(), RouterParams{});
    fixture.handshake();
    std::vector<geometry_msgs::msg::Twist> received;
    auto sub = fixture.node()->create_subscription<geometry_msgs::msg::Twist>(
        "cmd_vel", rclcpp::QoS(10),
    [&received](const geometry_msgs::msg::Twist & msg) {received.push_back(msg);});
    fixture.deliver_frame(kCmdVel, 0u, std::vector<std::uint8_t>(48u, 0x00u));
    rclcpp::spin_some(fixture.node());
    EXPECT_TRUE(received.empty()) << "a host-to-MCU topic never publishes";
}

TEST_F(RouterNode, HoldsBackpressureOnAWedgedLink)
{
    // A 256-byte frame cap holds one 200-byte string per unit, so units and
    // frames stay one to one while the session saturates and refuses.
    RouterFixture fixture("wedged", remapped(), RouterParams{}, 256u);
    fixture.handshake(0x01u, 7u, true, 0x09u);
    EXPECT_EQ(fixture.router().subscription_count(), 2u) << "cmd_vel and log_text subscribed";
    fixture.block_link();
    auto pub = fixture.node()->create_publisher<std_msgs::msg::String>("log_text", 10);
    std_msgs::msg::String text;
    text.data = std::string(200u, 'l');
    for (int i = 0; i < 4; ++i) {
    pub->publish(text);
    rclcpp::spin_some(fixture.node());
    }
    // Each poll sends one frame into the blocked session; the first three
    // fill its 515-byte MCU window and the fourth is refused, so the pass
    // stops with that frame still queued. The steps are wide enough for the
    // bandwidth budget to cover one frame each.
    for (int i = 0; i < 10 && fixture.router().queued(RouterFixture::kLogText) > 1u; ++i) {
    fixture.advance(100u);
    }
    EXPECT_EQ(fixture.router().queued(RouterFixture::kLogText),
          1u) << "the refused frame stays queued";
    EXPECT_EQ(fixture.router().dropped(RouterFixture::kLogText),
          0u) << "the scheduler sheds nothing";
    EXPECT_TRUE(fixture.drain().empty()) << "nothing leaves while wedged";
    fixture.allow_link();
    std::vector<std::vector<std::uint8_t>> user_units;
    for (int i = 0; i < 20 && user_units.size() < 4u; ++i) {
    fixture.advance(500u);
    for (const std::vector<std::uint8_t> & unit : fixture.drain()) {
      const std::vector<std::uint8_t> decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 4u && decoded[0] == RouterFixture::kLogText) {
        user_units.push_back(decoded);
      }
    }
    }
    ASSERT_EQ(user_units.size(), 4u) << "every queued frame flushes in order";
    EXPECT_EQ(user_units.front()[1], 0u) << "the first frame on a topic carries seq 0";
    EXPECT_EQ(user_units.back()[1], 3u) << "the refused frame goes out last with seq 3";
}

// The services proxy: MCU servers become ROS services, MCU calls become ROS
// clients, over the reliable channel (spec/protocol.md, 7.8).

// covers: SES-34, SVR-4, SVR-5, SVR-6
TEST_F(RouterNode, CallsTheMcuServiceAndReturnsItsAnswer)
{
  RouterFixture fixture("svc_call", remapped(), RouterParams{});
  fixture.handshake_with_services();
  auto client = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  auto future =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  // Spin until the packed empty request reaches the wire.
  const auto start = std::chrono::steady_clock::now();
  std::uint8_t req = 0u;
  bool found = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        req = decoded[4];
        found = true;
      }
    }
    if (found) {
      break;
    }
  }
  ASSERT_TRUE(found) << "the REQUEST never reached the wire";
  // The MCU answers ok with an empty message: packed true, empty string.
  fixture.deliver_frame(kServicesTopic, 0u,
    {kHelperSvcResponse, 0x00u, req, 0x00u, 0x01u, 0x00u});
  // The answer needs one spin to reach the client future.
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto response = future.get();
  EXPECT_TRUE(response->success);
  EXPECT_TRUE(response->message.empty());
}

// covers: SES-34, SVR-4, SVR-5
TEST_F(RouterNode, ForwardsTheMcuCallAndAnswersIt)
{
  RouterFixture fixture("svc_forward", remapped(), RouterParams{});
  bool called = false;
  auto server = fixture.node()->create_service<std_srvs::srv::Trigger>("trigger",
      [&called](
        const std::shared_ptr<std_srvs::srv::Trigger::Request>,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        called = true;
        response->success = true;
        response->message = "ok";
      });
  fixture.handshake_with_services();
  for (int i = 0; i < 50; ++i) {
    rclcpp::spin_some(fixture.node());
  }
  auto probe = fixture.node()->create_client<std_srvs::srv::Trigger>("trigger");
  ASSERT_TRUE(probe->wait_for_service(std::chrono::seconds(10)));
  for (int i = 0; i < 50; ++i) {
    rclcpp::spin_some(fixture.node());
  }
  // The MCU calls index 1 with an empty Trigger request under req 7.
  fixture.deliver_frame(kServicesTopic, 0u, {kHelperSvcRequest, 0x00u, 0x07u, 0x01u});
  const auto start = std::chrono::steady_clock::now();
  bool answered = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    if (!called) {
      continue;
    }
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 7u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcResponse && decoded[4] == 0x07u && decoded[5] == 0x00u)
      {
        // Packed true and "ok".
        const std::vector<std::uint8_t> want{0x01u, 0x02u, 'o', 'k'};
        EXPECT_EQ(
          std::vector<std::uint8_t>(decoded.begin() + 6u, decoded.end() - 2u), want);
        answered = true;
      }
    }
    if (answered) {
      break;
    }
  }
  EXPECT_TRUE(called) << "the ROS server never saw the call";
  EXPECT_TRUE(answered) << "the RESPONSE never reached the wire";
}

// covers: SES-34, SVR-8
TEST_F(RouterNode, TimesOutAnUnansweredMcuCall)
{
  RouterFixture fixture("svc_timeout", remapped(), RouterParams{});
  fixture.handshake_with_services();
  // No ROS server: the call goes out, and the host timeout answers busy.
  fixture.deliver_frame(kServicesTopic, 0u, {kHelperSvcRequest, 0x00u, 0x07u, 0x01u});
  for (int i = 0; i < 12; ++i) {
    fixture.advance(500u);
    rclcpp::spin_some(fixture.node());
    fixture.deliver_frame(kAckTopic, static_cast<std::uint8_t>(i), {kServicesTopic, 0x00u});
  }
  bool busy = false;
  for (const auto & unit : fixture.drain()) {
    const auto decoded = fixture.checked_decode(unit);
    if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
      decoded[2] == kHelperSvcResponse && decoded[4] == 0x07u && decoded[5] == 0x03u)
    {
      busy = true;
    }
  }
  EXPECT_TRUE(busy) << "no busy RESPONSE after the timeout";
}

// covers: SES-34, SVR-7
TEST_F(RouterNode, RefusesUnusableServices)
{
  // An unknown type is refused, and no ROS endpoint is created.
  RouterFixture bad_type("svc_badtype", remapped(), RouterParams{});
  bad_type.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0x4Cu));
  bad_type.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  bad_type.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> zero{};
  bad_type.deliver_frame(kServicesTopic, 0u,
    helper_service_declare(0u, 0x00u, zero.data(), "nope/msg/Nope", "bad"));
  bad_type.deliver_frame(kServicesTopic, 1u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  bool refused = false;
  for (const auto & unit : bad_type.drain()) {
    const auto decoded = bad_type.checked_decode(unit);
    if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
      decoded[2] == kHelperSvcStatus && decoded[4] == 0x00u && decoded[5] == 0x01u)
    {
      refused = true;
    }
  }
  EXPECT_TRUE(refused) << "no STATUS 1 for the unknown type";
  auto probe = bad_type.node()->create_client<std_srvs::srv::Trigger>("bad");
  EXPECT_FALSE(probe->wait_for_service(std::chrono::milliseconds(200)));
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
