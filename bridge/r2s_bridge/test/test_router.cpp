// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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

#include <action_msgs/msg/goal_status_array.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <sensor_msgs/srv/set_camera_info.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/int32_multi_array.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <tf2_msgs/action/lookup_transform.hpp>

#include "frame_helpers.hpp"
#include "manual_clock.hpp"
#include "r2s_bridge/action_types.hpp"
#include "r2s_bridge/converter.hpp"
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
    SessionConfig session_config = SessionConfig(), bool with_node_params = true,
    bool with_node_services = true)
  : max_frame_(max_frame), node_(std::make_shared<rclcpp::Node>(name, "robot1", options))
  {
    LogSink mcu_sink;
    if (with_mcu_sink) {
      mcu_sink = [this](SessionLog level, const std::string & text) {
          mcu_.emplace_back(level, text);
        };
    }
    NodeServiceInterfaces node_services;
    if (with_node_services) {
      node_services.base = node_->get_node_base_interface();
      node_services.graph = node_->get_node_graph_interface();
      node_services.services = node_->get_node_services_interface();
    }
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
    script_frame(topic_id, seq, payload);
    router_->poll();
  }

  void script_frame(
    std::uint8_t topic_id, std::uint8_t seq,
    const std::vector<std::uint8_t> & payload)
  {
    std::vector<std::uint8_t> decoded{topic_id, seq};
    decoded.insert(decoded.end(), payload.begin(), payload.end());
    transport_.script_read(helper_wire_of(helper_with_check(decoded)));
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

// covers: SES-34, SVR-7
TEST_F(RouterNode, RefusesServicesWithoutNodeInterfaces)
{
  RouterFixture fixture("svc_noiface", remapped(), RouterParams{},
    512u, true, SessionConfig(), true, false);
  fixture.handshake_with_services();
  for (int i = 0; i < 12; ++i) {
    fixture.advance(150u);
    rclcpp::spin_some(fixture.node());
  }
  // Neither the server nor the client endpoint exists.
  auto reset = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  auto trigger = fixture.node()->create_client<std_srvs::srv::Trigger>("trigger");
  EXPECT_FALSE(reset->wait_for_service(std::chrono::milliseconds(200)));
  EXPECT_FALSE(trigger->wait_for_service(std::chrono::milliseconds(200)));
  EXPECT_TRUE(logged_has(fixture.logged(), "no endpoint"));
}

// covers: SES-34, SVR-7
TEST_F(RouterNode, RefusesServicesWithUnusableNames)
{
  RouterFixture fixture("svc_badname", remapped(), RouterParams{});
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0x4Cu));
  fixture.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> hash = installed_service_hash("std_srvs/srv/Trigger");
  fixture.deliver_frame(kServicesTopic, 0u,
    helper_service_declare(0u, 0x00u, hash.data(), "std_srvs/srv/Trigger", "9bad"));
  fixture.deliver_frame(kServicesTopic, 1u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  // The rcl service name is unusable, so no endpoint is created.
  const auto services = fixture.node()->get_service_names_and_types();
  EXPECT_TRUE(services.find("9bad") == services.end());
  EXPECT_TRUE(logged_has(fixture.logged(), "no endpoint"));
}

// covers: SVR-4, SVR-5
TEST_F(RouterNode, AnswersTheSecondServerAfterTheFirst)
{
  RouterFixture fixture("svc_two", remapped(), RouterParams{});
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0x4Cu));
  fixture.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> hash = installed_service_hash("std_srvs/srv/Trigger");
  fixture.deliver_frame(kServicesTopic, 0u,
    helper_service_declare(0u, 0x00u, hash.data(), "std_srvs/srv/Trigger", "reset"));
  fixture.deliver_frame(kServicesTopic, 1u,
    helper_service_declare(1u, 0x00u, hash.data(), "std_srvs/srv/Trigger", "reset2"));
  fixture.deliver_frame(kServicesTopic, 2u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x02u, 0x01u});
  auto first = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  auto second = fixture.node()->create_client<std_srvs::srv::Trigger>("reset2");
  ASSERT_TRUE(first->wait_for_service(std::chrono::seconds(10)));
  ASSERT_TRUE(second->wait_for_service(std::chrono::seconds(10)));
  auto answer1 = first->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto start = std::chrono::steady_clock::now();
  bool requested = false;
  std::uint8_t first_req = 0u;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        first_req = decoded[4];
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  // The first answer clears the first route: the second answer scans past
  // it to the second route.
  fixture.deliver_frame(kServicesTopic, 0u,
    {kHelperSvcResponse, 0x00u, first_req, 0x00u, 0x01u, 0x00u});
  // The answer needs one spin to reach the client future.
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(answer1.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  // The ack frees the stream for the second route behind it.
  fixture.deliver_frame(kAckTopic, 0u, {kServicesTopic, 0x00u});
  // The second server runs its own route, told apart by its index.
  auto answer2 = second->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  requested = false;
  std::uint8_t second_req = 0u;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(20)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 7u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest && decoded[5] == 0x01u)
      {
        second_req = decoded[4];
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  fixture.deliver_frame(kServicesTopic, 1u,
    {kHelperSvcResponse, 0x01u, second_req, 0x00u, 0x01u, 0x00u});
  // The answer needs one spin to reach the client future.
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(answer2.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_TRUE(answer2.get()->success);
}

// covers: SVR-5, SVR-6
TEST_F(RouterNode, AnswersDefaultWhenTheMcuAnswerDoesNotUnpack)
{
  RouterFixture fixture("svc_badpayload", remapped(), RouterParams{});
  fixture.handshake_with_services();
  auto client = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  auto future =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto start = std::chrono::steady_clock::now();
  bool requested = false;
  std::uint8_t req = 0u;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        req = decoded[4];
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  // A boolean out of range never unpacks: the caller gets the default.
  fixture.deliver_frame(kServicesTopic, 0u, {kHelperSvcResponse, 0x00u, req, 0x00u, 0x02u});
  // The answer needs one spin to reach the client future.
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto response = future.get();
  EXPECT_FALSE(response->success);
  EXPECT_TRUE(response->message.empty());
}

// covers: SVR-5, SVR-6
TEST_F(RouterNode, AnswersDefaultWhenTheMcuReportsBusy)
{
  RouterFixture fixture("svc_msubusy", remapped(), RouterParams{});
  fixture.handshake_with_services();
  auto client = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  auto future =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto start = std::chrono::steady_clock::now();
  bool requested = false;
  std::uint8_t req = 0u;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        req = decoded[4];
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  // A busy MCU answers nothing itself: the caller gets the default.
  fixture.deliver_frame(kServicesTopic, 0u, {kHelperSvcResponse, 0x00u, req, 0x03u});
  // The answer needs one spin to reach the client future.
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto response = future.get();
  EXPECT_FALSE(response->success);
  EXPECT_TRUE(response->message.empty());
}

// covers: SVR-4, SVR-10
// covers: SVR-4, SVR-10
TEST_F(RouterNode, PromotesThePendingCallWhenTheAckArrives)
{
  RouterFixture fixture("svc_pending", remapped(), RouterParams{});
  fixture.handshake_with_services();
  auto client = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  auto first =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto start = std::chrono::steady_clock::now();
  bool requested = false;
  std::uint8_t first_req = 0u;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        first_req = decoded[4];
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  // A second call while the first is still outstanding waits behind it in
  // the router, so nothing new reaches the wire for it yet.
  auto second =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  for (int i = 0; i < 5; ++i) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      EXPECT_FALSE(decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
        << "the queued call never reaches the wire";
    }
  }
  // The MCU answers the first call without acking it: answered but still
  // outstanding, so the pumped second call waits in the pending slot.
  fixture.deliver_frame(kServicesTopic, 0u,
    {kHelperSvcResponse, 0x00u, first_req, 0x00u, 0x01u, 0x00u});
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(first.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_TRUE(first.get()->success) << "the first answer completes its caller";
  for (int i = 0; i < 2; ++i) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      EXPECT_FALSE(decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest && decoded[4] != first_req)
        << "the pending call never reaches the wire";
    }
  }
  // The late ack frees the stream and the pending call goes out next.
  fixture.deliver_frame(kAckTopic, 0u, {kServicesTopic, 0x00u});
  const auto stop = std::chrono::steady_clock::now();
  bool promoted = false;
  std::uint8_t second_req = 0u;
  while (std::chrono::steady_clock::now() - stop < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest && decoded[4] != first_req)
      {
        EXPECT_EQ(decoded[3], 0x01u) << "the promoted call takes the next rel_seq";
        second_req = decoded[4];
        promoted = true;
      }
    }
    if (promoted) {
      break;
    }
  }
  ASSERT_TRUE(promoted) << "the ack promotes the pending call";
  // The MCU answers the promoted call under the next rel_seq of its own
  // stream; the answer completes the waiting future.
  fixture.deliver_frame(kServicesTopic, 0u,
    {kHelperSvcResponse, 0x01u, second_req, 0x00u, 0x01u, 0x00u});
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(second.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto response = second.get();
  EXPECT_TRUE(response->success);
  EXPECT_TRUE(response->message.empty());
  (void)first;
}

// covers: SVR-8, SVR-10
TEST_F(RouterNode, DropsTheUnackedCallAndPromotesThePendingOne)
{
  RouterFixture fixture("svc_giveup", remapped(), RouterParams{});
  fixture.handshake_with_services();
  auto client = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  auto first =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto start = std::chrono::steady_clock::now();
  bool requested = false;
  std::uint8_t first_req = 0u;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        first_req = decoded[4];
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  auto second =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  // The MCU answers the first call without acking it: answered but still
  // outstanding, so the pumped second call waits in the pending slot.
  fixture.deliver_frame(kServicesTopic, 0u,
    {kHelperSvcResponse, 0x00u, first_req, 0x00u, 0x01u, 0x00u});
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(first.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_TRUE(first.get()->success) << "the first answer completes its caller";
  // Acks for a stranger rel_seq keep the link alive without freeing the
  // stream. The first three polls only resend the first frame; the fourth
  // spends the last retry and drops it, promoting the pending call.
  for (int i = 0; i < 3; ++i) {
    fixture.advance(500u);
    rclcpp::spin_some(fixture.node());
    fixture.deliver_frame(
      kAckTopic, static_cast<std::uint8_t>(i), {kServicesTopic, 0x07u});
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      EXPECT_FALSE(decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest && decoded[4] != first_req)
        << "the pending call never reaches the wire";
    }
  }
  fixture.advance(500u);
  rclcpp::spin_some(fixture.node());
  fixture.deliver_frame(kAckTopic, 3u, {kServicesTopic, 0x07u});
  EXPECT_TRUE(logged_has(fixture.logged(), "no ack for the service frame"))
    << "the unacked frame is dropped and counted";
  // The drop promotes the pending call: it reaches the wire now.
  const std::uint8_t second_req =
    static_cast<std::uint8_t>(first_req + 1u);
  bool promoted = false;
  for (int i = 4; i < 6 && !promoted; ++i) {
    fixture.advance(500u);
    rclcpp::spin_some(fixture.node());
    fixture.deliver_frame(
      kAckTopic, static_cast<std::uint8_t>(i), {kServicesTopic, 0x07u});
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest && decoded[4] == second_req)
      {
        EXPECT_EQ(decoded[3], 0x01u) << "the promoted call takes the next rel_seq";
        promoted = true;
      }
    }
  }
  ASSERT_TRUE(promoted) << "the drop promotes the pending call";
  fixture.deliver_frame(kAckTopic, 6u, {kServicesTopic, 0x01u});
  fixture.deliver_frame(kServicesTopic, 0u,
    {kHelperSvcResponse, 0x01u, second_req, 0x00u, 0x01u, 0x00u});
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(second.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto response = second.get();
  EXPECT_TRUE(response->success);
  EXPECT_TRUE(response->message.empty());
  (void)first;
}

// covers: SVR-4
TEST_F(RouterNode, RefusesTheCallThatExceedsTheFrameCap)
{
  RouterFixture fixture("svc_big", remapped(), RouterParams{});
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0x4Cu));
  fixture.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> hash =
    installed_service_hash("sensor_msgs/srv/SetCameraInfo");
  fixture.deliver_frame(kServicesTopic, 0u,
    helper_service_declare(0u, 0x00u, hash.data(), "sensor_msgs/srv/SetCameraInfo", "cam"));
  fixture.deliver_frame(kServicesTopic, 1u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  auto client = fixture.node()->create_client<sensor_msgs::srv::SetCameraInfo>("cam");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  // A packed request larger than the announced frame fits no channel, so
  // the send refuses it straight away.
  auto request = std::make_shared<sensor_msgs::srv::SetCameraInfo::Request>();
  request->camera_info.distortion_model = std::string(600u, 'd');
  auto future = client->async_send_request(request);
  const auto start = std::chrono::steady_clock::now();
  bool answered = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      answered = true;
      break;
    }
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      EXPECT_FALSE(decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
        << "the oversize call never reaches the wire";
    }
  }
  ASSERT_TRUE(answered);
  const auto refused = future.get();
  EXPECT_FALSE(refused->success);
  EXPECT_TRUE(refused->status_message.empty());
  EXPECT_TRUE(logged_has(fixture.logged(), "refused the ROS call"));
}

// covers: SVR-4
TEST_F(RouterNode, DropsTheFourthCallWhileOneIsInflight)
{
  RouterFixture fixture("svc_overload", remapped(), RouterParams{});
  fixture.handshake_with_services();
  auto client = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  auto first =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto start = std::chrono::steady_clock::now();
  bool requested = false;
  std::uint8_t first_req = 0u;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        first_req = decoded[4];
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  // Three more calls queue behind the outstanding one; the fourth exceeds
  // the queue and is answered empty straight away.
  auto queued_a =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  auto queued_b =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  auto queued_c =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  auto dropped =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto stop = std::chrono::steady_clock::now();
  bool answered = false;
  while (std::chrono::steady_clock::now() - stop < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    if (dropped.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      answered = true;
      break;
    }
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      EXPECT_FALSE(decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest && decoded[4] != first_req)
        << "the queued calls never reach the wire";
    }
  }
  ASSERT_TRUE(answered);
  const auto refused = dropped.get();
  EXPECT_FALSE(refused->success);
  EXPECT_TRUE(refused->message.empty());
  EXPECT_TRUE(logged_has(fixture.logged(), "overloaded"));
  EXPECT_FALSE(
    queued_a.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
  EXPECT_FALSE(
    queued_b.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
  EXPECT_FALSE(
    queued_c.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
  (void)first;
}

// covers: SVR-5, SVR-6
TEST_F(RouterNode, DropsTheMcuCallThatDoesNotUnpack)
{
  RouterFixture fixture("svc_badmcu", remapped(), RouterParams{});
  fixture.handshake_with_services();
  // A junk byte behind the empty Trigger request never unpacks: the router
  // reports a bad payload without calling ROS.
  fixture.deliver_frame(kServicesTopic, 0u, {kHelperSvcRequest, 0x00u, 0x07u, 0x01u, 0x02u});
  const auto start = std::chrono::steady_clock::now();
  bool answered = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 7u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcResponse && decoded[4] == 0x07u && decoded[5] == 0x02u)
      {
        answered = true;
      }
    }
    if (answered) {
      break;
    }
  }
  EXPECT_TRUE(answered) << "no bad-payload RESPONSE for the unpackable call";
  EXPECT_TRUE(logged_has(fixture.logged(), "dropped MCU call"));
}

// covers: SVR-4
TEST_F(RouterNode, RefusesTheRosAnswerThatExceedsTheFrameCap)
{
  RouterFixture fixture("svc_biganswer", remapped(), RouterParams{});
  auto server = fixture.node()->create_service<std_srvs::srv::Trigger>("trigger",
      [](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
      std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        response->success = true;
        response->message = std::string(1000u, 'd');
    });
  fixture.handshake_with_services();
  for (int i = 0; i < 50; ++i) {
    rclcpp::spin_some(fixture.node());
  }
  // The ROS answer packs, but the frame it needs fits no channel, so the
  // send refuses it and nothing reaches the wire.
  fixture.deliver_frame(kServicesTopic, 0u, {kHelperSvcRequest, 0x00u, 0x07u, 0x01u});
  const auto start = std::chrono::steady_clock::now();
  bool refused = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    if (logged_has(fixture.logged(), "refused the ROS answer")) {
      refused = true;
      break;
    }
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      EXPECT_FALSE(decoded.size() >= 7u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcResponse && decoded[4] == 0x07u)
        << "the refused answer never reaches the wire";
    }
  }
  EXPECT_TRUE(refused);
  (void)server;
}

// covers: SVR-4, SVR-10
TEST_F(RouterNode, DropsARosRequestThatDoesNotPack)
{
  RouterFixture fixture("svc_badros", remapped(), RouterParams{});
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0x4Cu));
  fixture.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> hash =
    installed_service_hash("sensor_msgs/srv/SetCameraInfo");
  fixture.deliver_frame(kServicesTopic, 0u,
    helper_service_declare(0u, 0x00u, hash.data(), "sensor_msgs/srv/SetCameraInfo", "cam"));
  fixture.deliver_frame(kServicesTopic, 1u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  auto client = fixture.node()->create_client<sensor_msgs::srv::SetCameraInfo>("cam");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  // A string past the wire length passes the middleware but never packs,
  // so the take drops the call straight away.
  auto request = std::make_shared<sensor_msgs::srv::SetCameraInfo::Request>();
  request->camera_info.distortion_model = std::string(70000u, 'd');
  auto future = client->async_send_request(request);
  // Spin until the take drops it: no REQUEST reaches the wire.
  const auto start = std::chrono::steady_clock::now();
  bool answered = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      answered = true;
      break;
    }
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      EXPECT_FALSE(decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
        << "the unpackable call never reaches the wire";
    }
  }
  ASSERT_TRUE(answered);
  const auto dropped = future.get();
  EXPECT_FALSE(dropped->success);
  EXPECT_TRUE(dropped->status_message.empty());
  EXPECT_TRUE(logged_has(fixture.logged(), "dropped ROS request"));
}

// covers: SVR-8
TEST_F(RouterNode, AnswersEmptyWhenTheMcuNeverAnswers)
{
  RouterFixture fixture("svc_rostimeout", remapped(), RouterParams{});
  fixture.handshake_with_services();
  auto client = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  auto future =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto start = std::chrono::steady_clock::now();
  bool requested = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  // The MCU stays silent: acks keep the link alive past the call timeout.
  for (int i = 0; i < 12; ++i) {
    fixture.advance(500u);
    rclcpp::spin_some(fixture.node());
    fixture.deliver_frame(kAckTopic, static_cast<std::uint8_t>(i), {kServicesTopic, 0x00u});
  }
  ASSERT_EQ(future.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  const auto response = future.get();
  EXPECT_FALSE(response->success);
  EXPECT_TRUE(response->message.empty());
}

// covers: SVR-5
TEST_F(RouterNode, ReportsBusyWhenTheRosAnswerDoesNotPack)
{
  bool called = false;
  RouterFixture fixture("svc_nulanswer", remapped(), RouterParams{});
  // A message past the wire length passes the middleware but never packs,
  // so the router answers the MCU call busy instead.
  auto server = fixture.node()->create_service<std_srvs::srv::Trigger>("trigger",
      [&called](
        const std::shared_ptr<std_srvs::srv::Trigger::Request>,
        std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        called = true;
        response->success = true;
        response->message = std::string(70000u, 'd');
    });
  fixture.handshake_with_services();
  for (int i = 0; i < 50; ++i) {
    rclcpp::spin_some(fixture.node());
  }
  fixture.deliver_frame(kServicesTopic, 0u, {kHelperSvcRequest, 0x00u, 0x07u, 0x01u});
  const auto start = std::chrono::steady_clock::now();
  bool busy = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcResponse && decoded[4] == 0x07u && decoded[5] == 0x03u)
      {
        busy = true;
      }
    }
    if (busy) {
      break;
    }
  }
  EXPECT_TRUE(called) << "the ROS server never saw the call";
  EXPECT_TRUE(busy) << "no busy RESPONSE for the unpackable answer";
}

// covers: SVR-5, SVR-10
TEST_F(RouterNode, HoldsTheRosAnswerWhileTheLinkIsBusy)
{
  RouterFixture fixture("svc_holdanswer", remapped(), RouterParams{});
  auto server = fixture.node()->create_service<std_srvs::srv::Trigger>("trigger",
      [](const std::shared_ptr<std_srvs::srv::Trigger::Request>,
      std::shared_ptr<std_srvs::srv::Trigger::Response> response) {
        response->success = true;
        response->message = "ok";
    });
  fixture.handshake_with_services();
  for (int i = 0; i < 50; ++i) {
    rclcpp::spin_some(fixture.node());
  }
  auto caller = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(caller->wait_for_service(std::chrono::seconds(10)));
  auto outstanding =
    caller->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto start = std::chrono::steady_clock::now();
  bool requested = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  // The MCU calls while the host call is outstanding: the ROS answer packs,
  // but the busy stream holds it behind the outstanding frame.
  fixture.deliver_frame(kServicesTopic, 1u, {kHelperSvcRequest, 0x00u, 0x07u, 0x01u});
  // The ROS round trip takes a few polls; retries of the host frame pass
  // through while the held answer stays queued behind it.
  for (int i = 0; i < 25; ++i) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      EXPECT_FALSE(decoded.size() >= 7u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcResponse && decoded[4] == 0x07u)
        << "the held answer never reaches the wire";
    }
  }
  // The ack frees the stream and the held answer goes out next.
  fixture.deliver_frame(kAckTopic, 0u, {kServicesTopic, 0x00u});
  const auto stop = std::chrono::steady_clock::now();
  bool promoted = false;
  while (std::chrono::steady_clock::now() - stop < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 7u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcResponse && decoded[4] == 0x07u && decoded[5] == 0x00u)
      {
        promoted = true;
      }
    }
    if (promoted) {
      break;
    }
  }
  EXPECT_TRUE(promoted) << "the ack promotes the held answer";
  (void)outstanding;
  (void)server;
}

// covers: SVR-5, SVR-8
TEST_F(RouterNode, IgnoresALateResponseAfterTheCallTimedOut)
{
  RouterFixture fixture("svc_late", remapped(), RouterParams{});
  fixture.handshake_with_services();
  auto client = fixture.node()->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  auto first =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto start = std::chrono::steady_clock::now();
  bool requested = false;
  std::uint8_t first_req = 0u;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest)
      {
        first_req = decoded[4];
        requested = true;
      }
    }
    if (requested) {
      break;
    }
  }
  ASSERT_TRUE(requested);
  // Wrong-rel acks keep the link alive without freeing the stream: the
  // call times out while the MCU stays silent.
  for (int i = 0; i < 11; ++i) {
    fixture.advance(500u);
    rclcpp::spin_some(fixture.node());
    fixture.deliver_frame(
      kAckTopic, static_cast<std::uint8_t>(i), {kServicesTopic, 0x07u});
    if (first.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
      break;
    }
  }
  ASSERT_EQ(first.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_FALSE(first.get()->success) << "the silent call times out empty";
  EXPECT_TRUE(logged_has(fixture.logged(), "the MCU never answered"));
  // The MCU's late RESPONSE matches nothing and is ignored.
  fixture.deliver_frame(kServicesTopic, 0u,
    {kHelperSvcResponse, 0x00u, first_req, 0x00u, 0x01u, 0x00u});
  rclcpp::spin_some(fixture.node());
  // The stream is not wedged: the ack frees it and a new call completes.
  fixture.deliver_frame(kAckTopic, 11u, {kServicesTopic, 0x00u});
  auto second =
    client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto stop = std::chrono::steady_clock::now();
  bool promoted = false;
  std::uint8_t second_req = 0u;
  while (std::chrono::steady_clock::now() - stop < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kServicesTopic &&
        decoded[2] == kHelperSvcRequest && decoded[4] != first_req)
      {
        second_req = decoded[4];
        promoted = true;
      }
    }
    if (promoted) {
      break;
    }
  }
  ASSERT_TRUE(promoted);
  fixture.deliver_frame(kServicesTopic, 1u,
    {kHelperSvcResponse, 0x01u, second_req, 0x00u, 0x01u, 0x00u});
  rclcpp::spin_some(fixture.node());
  ASSERT_EQ(second.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_TRUE(second.get()->success);
}

// The bridge as the ROS 2 action server for one MCU action: goals reach the
// MCU over the reliable channel, and feedback, cancel and result come back
// (spec/protocol.md, 7.9; SES-35).
// covers: SES-35, ACT-2, ACT-3

std::array<std::uint8_t, 8> installed_action_hash(const std::string & type)
{
  const ActionTypes types(type);
  return types.type_hash();
}

// The link never times out under the test's manual clock: each poll
// advances it 20 ms, so a default timeout would drop the session mid-goal.
SessionConfig action_session_config()
{
  SessionConfig config;
  config.link_timeout_ms = 3600000;
  return config;
}

// A handshake announcing one LookupTransform action "spin", with the
// installed hash. Time stays off the HELLO so no PING disturbs the capture.
void handshake_with_actions(RouterFixture & fixture)
{
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0xCCu));
  fixture.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kServicesTopic, 0u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> hash =
    installed_action_hash("tf2_msgs/action/LookupTransform");
  fixture.deliver_frame(kHelperActions, 0u,
    helper_action_declare(0u, hash.data(), "tf2_msgs/action/LookupTransform", "spin"));
  fixture.deliver_frame(kHelperActions, 1u,
    std::vector<std::uint8_t>{kHelperActStatus, 0x00u, 0x01u, 0x01u});
}

// One LookupTransform goal asking for "base" in "odom".
tf2_msgs::action::LookupTransform_Goal make_spin_goal()
{
  tf2_msgs::action::LookupTransform_Goal goal;
  goal.target_frame = "base";
  goal.source_frame = "odom";
  return goal;
}

// The packed wire form of one introspection struct, for the scripted MCU.
std::vector<std::uint8_t> pack_action(
  const std::string & type, const void *storage, Converter & converter)
{
  std::vector<std::uint8_t> packed;
  const ConvertResult result = converter.pack_from_struct(type, false, storage, packed);
  EXPECT_EQ(result.fail, ConvertFail::kNone);
  return packed;
}

// Spins the node and the router until a future is ready, or the timeout
// expires: ROS futures only complete while the node spins.
template<typename FutureT>
bool spin_until_ready(RouterFixture & fixture, FutureT & future)
{
  const auto start = std::chrono::steady_clock::now();
  while (future.wait_for(std::chrono::seconds(0)) != std::future_status::ready &&
    std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
  {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// Spins until the wire carries an ACT_GOAL, or the timeout expires.
bool wait_for_wire_goal(
  RouterFixture & fixture, std::uint8_t & goal_seq, std::vector<std::uint8_t> & payload)
{
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 7u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActGoal)
      {
        goal_seq = decoded[4];
        payload = std::vector<std::uint8_t>(decoded.begin() + 6, decoded.end() - 2);
        return true;
      }
    }
  }
  return false;
}

// covers: SES-35, ACT-2, ACT-3, ACT-8
TEST_F(RouterNode, AcceptsAGoalAndStreamsFeedbackAndResult)
{
  RouterFixture fixture("act_goal", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));
  Converter converter;
  const auto spin_goal = make_spin_goal();
  const auto want = pack_action(
    "tf2_msgs/action/LookupTransform_Goal", &spin_goal, converter);

  bool feedback_seen = false;
  rclcpp_action::Client<tf2_msgs::action::LookupTransform>::SendGoalOptions options;
  options.feedback_callback =
    [&feedback_seen](rclcpp_action::ClientGoalHandle<
      tf2_msgs::action::LookupTransform>::SharedPtr,
    const std::shared_ptr<const tf2_msgs::action::LookupTransform_Feedback> &) {
      feedback_seen = true;
    };
  auto goal_handle = client->async_send_goal(make_spin_goal(), options);
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  EXPECT_EQ(goal_seq, 0x00u);
  EXPECT_EQ(payload, want);
  // The MCU accepts goal 0.
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  const auto handle = goal_handle.get();
  ASSERT_NE(handle, nullptr);

  // The status array names the live goal: executing now, succeeded at
  // the end. The bridge publishes it on change and periodically.
  std::vector<action_msgs::msg::GoalStatusArray> statuses;
  auto status_sub = fixture.node()->create_subscription<action_msgs::msg::GoalStatusArray>(
    "spin/_action/status", 10,
    [&statuses](action_msgs::msg::GoalStatusArray::SharedPtr message) {
      statuses.push_back(*message);
    });
  const auto status_shows = [&](std::int8_t want) {
      const auto start = std::chrono::steady_clock::now();
      while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
        fixture.advance(20u);
        rclcpp::spin_some(fixture.node());
        for (const auto & array : statuses) {
          for (const auto & entry : array.status_list) {
            if (entry.goal_info.goal_id.uuid == handle->get_goal_id() &&
              entry.status == want)
            {
              return true;
            }
          }
        }
      }
      return false;
    };
  EXPECT_TRUE(status_shows(2)) << "the status array never showed the goal executing";

  // Empty feedback rides best effort with no ack. Drain first: the ack
  // answering the MCU's GOAL_RESP is still queued behind the link.
  static_cast<void>(fixture.drain());
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActFeedback, 0x00u, 0x00u});
  const auto start = std::chrono::steady_clock::now();
  while (!feedback_seen && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  EXPECT_TRUE(feedback_seen);
  bool acked = false;
  for (const auto & unit : fixture.drain()) {
    const auto decoded = fixture.checked_decode(unit);
    if (!decoded.empty() && decoded[0] == kAckTopic) {
      acked = true;
    }
  }
  EXPECT_FALSE(acked) << "feedback is never acked";

  // The result completes the ROS goal as succeeded.
  tf2_msgs::action::LookupTransform_Result result;
  result.transform.header.frame_id = "b";
  const auto packed = pack_action(
    "tf2_msgs/action/LookupTransform_Result", &result, converter);
  auto result_future = client->async_get_result(handle);
  std::vector<std::uint8_t> frame{kHelperActResult, 0x01u, 0x00u, 0x04u};
  frame.insert(frame.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 4u, frame);
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  const auto wrapped = result_future.get();
  EXPECT_EQ(wrapped.code, rclcpp_action::ResultCode::SUCCEEDED);
  EXPECT_EQ(wrapped.result->transform.header.frame_id, "b");
  EXPECT_TRUE(status_shows(4)) << "the status array never showed the goal succeeded";
}

// covers: SES-35, ACT-4
TEST_F(RouterNode, RejectsAGoalTheMcuRefuses)
{
  RouterFixture fixture("act_reject", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  // The MCU refuses goal 0.
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  EXPECT_EQ(goal_handle.get(), nullptr) << "a refused goal completes with no handle";
}

// covers: SES-35
TEST_F(RouterNode, RejectsASecondGoalWhileOneIsActive)
{
  RouterFixture fixture("act_second", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto first = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, first));
  ASSERT_NE(first.get(), nullptr);

  // A second goal while one is active never reaches the wire.
  auto second = client->async_send_goal(make_spin_goal());
  const auto start = std::chrono::steady_clock::now();
  bool second_wire = false;
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 7u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActGoal)
      {
        second_wire = true;
      }
    }
  }
  EXPECT_FALSE(second_wire) << "the bridge rejects the second goal itself";
  ASSERT_TRUE(spin_until_ready(fixture, second));
  EXPECT_EQ(second.get(), nullptr) << "a rejected goal completes with no handle";
}

// covers: SES-35, ACT-5, ACT-8
TEST_F(RouterNode, CancelsAnActiveGoal)
{
  RouterFixture fixture("act_cancel", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  const auto handle = goal_handle.get();
  ASSERT_NE(handle, nullptr);

  auto cancel = client->async_cancel_goal(handle);
  // The cancel reaches the wire for goal 0.
  bool cancel_wire = false;
  const auto start = std::chrono::steady_clock::now();
  while (!cancel_wire && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActCancel && decoded[4] == 0x00u)
      {
        cancel_wire = true;
      }
    }
  }
  ASSERT_TRUE(cancel_wire) << "the CANCEL never reached the wire";
  // A second cancel while the first is still with the MCU answers at once
  // instead of queuing behind it.
  auto cancel_again = client->async_cancel_goal(handle);
  ASSERT_TRUE(spin_until_ready(fixture, cancel_again));
  EXPECT_EQ(cancel_again.get()->return_code, 0)
    << "an already-canceling goal reports no new cancel";
  // A CANCEL_RESP for another seq applies to no pending cancel.
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActCancelResp, 0x01u, 0x09u, 0x00u});
  EXPECT_FALSE(cancel.wait_for(std::chrono::seconds(0)) == std::future_status::ready);
  // The MCU accepts the cancel.
  fixture.deliver_frame(kHelperActions, 4u, {kHelperActCancelResp, 0x02u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 4u, {kHelperActions, 0x02u});
  ASSERT_TRUE(spin_until_ready(fixture, cancel));

  // The canceled result completes the ROS goal as canceled.
  Converter converter;
  tf2_msgs::action::LookupTransform_Result result;
  const auto packed = pack_action(
    "tf2_msgs/action/LookupTransform_Result", &result, converter);
  auto result_future = client->async_get_result(handle);
  std::vector<std::uint8_t> frame{kHelperActResult, 0x03u, 0x00u, 0x05u};
  frame.insert(frame.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 5u, frame);
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::CANCELED);
}

// covers: SES-35, ACT-5
TEST_F(RouterNode, CancelsAllGoalsWithAZeroUuid)
{
  RouterFixture fixture("act_cancelall", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  ASSERT_NE(goal_handle.get(), nullptr);

  // Cancel-all carries a zero uuid: it matches the live goal by id and by
  // the zero stamp meaning any time.
  auto cancel = client->async_cancel_all_goals();
  bool cancel_wire = false;
  const auto start = std::chrono::steady_clock::now();
  while (!cancel_wire && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActCancel && decoded[4] == 0x00u)
      {
        cancel_wire = true;
      }
    }
  }
  ASSERT_TRUE(cancel_wire) << "the cancel-all never reached the wire";
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActCancelResp, 0x01u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x01u});
  ASSERT_TRUE(spin_until_ready(fixture, cancel));
}

// covers: SES-35, ACT-9
TEST_F(RouterNode, AbortsTheGoalWhenTheLinkResets)
{
  RouterFixture fixture("act_abort", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  const auto handle = goal_handle.get();
  ASSERT_NE(handle, nullptr);
  auto result_future = client->async_get_result(handle);

  // The MCU resets mid-goal: a new HELLO and a full re-handshake. The
  // empty parameter and service walks end at index 0; the action walk ends
  // at index 1 after the one DECLARE.
  fixture.deliver_frame(kHelperControl, 5u, helper_hello(0x4445u, 1u, 512u, 0xCCu));
  fixture.deliver_frame(kHelperControl, 6u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 1u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kServicesTopic, 1u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> hash =
    installed_action_hash("tf2_msgs/action/LookupTransform");
  fixture.deliver_frame(kHelperActions, 5u,
    helper_action_declare(0u, hash.data(), "tf2_msgs/action/LookupTransform", "spin"));
  fixture.deliver_frame(kHelperActions, 6u,
    std::vector<std::uint8_t>{kHelperActStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.router().diagnostics().link_state, "connected")
    << "the re-handshake never completed";
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::ABORTED);
  // Asking again after the re-handshake finds no trace of the old goal: the
  // client itself refuses the forgotten goal.
  EXPECT_THROW(
    {[[maybe_unused]] auto retake = client->async_get_result(handle);},
    rclcpp_action::exceptions::UnknownGoalHandleError);
}

// Spins until the wire carries an ACT_GOAL for one action, or the timeout
// expires.
bool wait_for_indexed_goal(
  RouterFixture & fixture, std::uint8_t & index, std::uint8_t & goal_seq)
{
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 7u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActGoal)
      {
        index = decoded[5];
        goal_seq = decoded[4];
        return true;
      }
    }
  }
  return false;
}

// covers: SES-35, ACT-7
TEST_F(RouterNode, RefusesAnUnknownActionType)
{
  RouterFixture fixture("act_badtype", remapped(), RouterParams{},
    512u, true, action_session_config());
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0xCCu));
  fixture.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kServicesTopic, 0u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> zero{};
  fixture.deliver_frame(kHelperActions, 0u,
    helper_action_declare(0u, zero.data(), "nope/action/Nope", "bad"));
  fixture.deliver_frame(kHelperActions, 1u,
    std::vector<std::uint8_t>{kHelperActStatus, 0x00u, 0x01u, 0x01u});
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "bad");
  EXPECT_FALSE(client->wait_for_action_server(std::chrono::milliseconds(200)));
  EXPECT_TRUE(logged_has(fixture.logged(), "unknown type"));
}

// covers: SES-35, ACT-1
TEST_F(RouterNode, RefusesAnActionWithAMismatchedHash)
{
  RouterFixture fixture("act_badhash", remapped(), RouterParams{},
    512u, true, action_session_config());
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0xCCu));
  fixture.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kServicesTopic, 0u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  // A known type with a corrupted hash is a mismatch, not an unknown type:
  // the bridge knows the type but the MCU's copy differs.
  std::array<std::uint8_t, 8> hash =
    installed_action_hash("tf2_msgs/action/LookupTransform");
  hash[0] ^= 0xFFu;
  fixture.deliver_frame(kHelperActions, 0u,
    helper_action_declare(0u, hash.data(), "tf2_msgs/action/LookupTransform", "spin"));
  fixture.deliver_frame(kHelperActions, 1u,
    std::vector<std::uint8_t>{kHelperActStatus, 0x00u, 0x01u, 0x01u});
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  EXPECT_FALSE(client->wait_for_action_server(std::chrono::milliseconds(200)));
}

// covers: SES-35, ACT-7
TEST_F(RouterNode, RefusesAnActionWithAnUnusableName)
{
  RouterFixture fixture("act_badname", remapped(), RouterParams{},
    512u, true, action_session_config());
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0xCCu));
  fixture.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kServicesTopic, 0u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  // The type loads, so the walk accepts the DECLARE; the name cannot name
  // a ROS action, so no endpoint is created for it.
  const std::array<std::uint8_t, 8> hash =
    installed_action_hash("tf2_msgs/action/LookupTransform");
  fixture.deliver_frame(kHelperActions, 0u,
    helper_action_declare(0u, hash.data(), "tf2_msgs/action/LookupTransform", "9bad!"));
  fixture.deliver_frame(kHelperActions, 1u,
    std::vector<std::uint8_t>{kHelperActStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_TRUE(logged_has(fixture.logged(), "no endpoint"));
}

// covers: SES-35
TEST_F(RouterNode, RefusesActionsWithoutNodeInterfaces)
{
  RouterFixture fixture("act_noiface", remapped(), RouterParams{},
    512u, true, action_session_config(), true, false);
  handshake_with_actions(fixture);
  // Same-tick polls hit the repair spacing: the route still fails, so the
  // retry waits instead of hammering the creation.
  for (int i = 0; i < 3; ++i) {
    fixture.advance(0u);
    rclcpp::spin_some(fixture.node());
  }
  for (int i = 0; i < 12; ++i) {
    fixture.advance(150u);
    rclcpp::spin_some(fixture.node());
  }
  // Neither the action server nor any ROS endpoint exists.
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  EXPECT_FALSE(client->wait_for_action_server(std::chrono::milliseconds(200)));
  EXPECT_TRUE(logged_has(fixture.logged(), "no endpoint"));
}

// A handshake announcing two LookupTransform actions, "spin" and "spin2".
void handshake_with_two_actions(RouterFixture & fixture)
{
  fixture.deliver_frame(kHelperControl, 0u, helper_hello(0x4444u, 1u, 512u, 0xCCu));
  fixture.deliver_frame(kHelperControl, 1u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.deliver_frame(kParamsTopic, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kServicesTopic, 0u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> hash =
    installed_action_hash("tf2_msgs/action/LookupTransform");
  fixture.deliver_frame(kHelperActions, 0u,
    helper_action_declare(0u, hash.data(), "tf2_msgs/action/LookupTransform", "spin"));
  fixture.deliver_frame(kHelperActions, 1u,
    helper_action_declare(1u, hash.data(), "tf2_msgs/action/LookupTransform", "spin2"));
  fixture.deliver_frame(kHelperActions, 2u,
    std::vector<std::uint8_t>{kHelperActStatus, 0x00u, 0x02u, 0x01u});
}

// covers: SES-35, ACT-2, ACT-3, ACT-4, ACT-6, ACT-8
TEST_F(RouterNode, RunsTwoActionsWithUniqueWireGoals)
{
  RouterFixture fixture("act_two", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_two_actions(fixture);
  auto first_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  auto second_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin2");
  ASSERT_TRUE(first_client->wait_for_action_server(std::chrono::seconds(10)));
  ASSERT_TRUE(second_client->wait_for_action_server(std::chrono::seconds(10)));

  auto first = first_client->async_send_goal(make_spin_goal());
  std::uint8_t first_index = 0xFFu;
  std::uint8_t first_seq = 0xFFu;
  ASSERT_TRUE(wait_for_indexed_goal(fixture, first_index, first_seq));
  EXPECT_EQ(first_index, 0x00u);
  EXPECT_EQ(first_seq, 0x00u);
  // Ack promptly like a real MCU: the reliable channel carries one frame
  // at a time, so the second GOAL waits behind an unacked first.
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x00u});
  auto second = second_client->async_send_goal(make_spin_goal());
  // Wait for the second action's GOAL specifically: the first goal's
  // reliable retransmits may still be on the wire.
  std::uint8_t second_index = 0xFFu;
  std::uint8_t second_seq = 0xFFu;
  const auto start = std::chrono::steady_clock::now();
  while (second_index != 0x01u &&
    std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
  {
    if (!wait_for_indexed_goal(fixture, second_index, second_seq)) {
      break;
    }
  }
  EXPECT_EQ(second_index, 0x01u) << "each action walks its own wire index";
  EXPECT_EQ(second_seq, 0x01u) << "the bridge skips seqs live on other actions";
  fixture.deliver_frame(kAckTopic, 4u, {kHelperActions, 0x01u});

  // The MCU accepts and completes both, each answered on its own seq.
  fixture.deliver_frame(kHelperActions, 5u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kHelperActions, 6u, {kHelperActGoalResp, 0x01u, 0x01u, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, first));
  ASSERT_TRUE(spin_until_ready(fixture, second));
  ASSERT_NE(first.get(), nullptr);
  ASSERT_NE(second.get(), nullptr);

  Converter converter;
  tf2_msgs::action::LookupTransform_Result result;
  const auto packed = pack_action(
    "tf2_msgs/action/LookupTransform_Result", &result, converter);
  auto first_result = first_client->async_get_result(first.get());
  auto second_result = second_client->async_get_result(second.get());
  std::vector<std::uint8_t> frame0{kHelperActResult, 0x02u, 0x00u, 0x04u};
  frame0.insert(frame0.end(), packed.begin(), packed.end());
  std::vector<std::uint8_t> frame1{kHelperActResult, 0x03u, 0x01u, 0x04u};
  frame1.insert(frame1.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 7u, frame0);
  fixture.deliver_frame(kHelperActions, 8u, frame1);
  ASSERT_TRUE(spin_until_ready(fixture, first_result));
  ASSERT_TRUE(spin_until_ready(fixture, second_result));
  EXPECT_EQ(first_result.get().code, rclcpp_action::ResultCode::SUCCEEDED);
  EXPECT_EQ(second_result.get().code, rclcpp_action::ResultCode::SUCCEEDED);
}

// covers: SES-35, ACT-1, ACT-4, ACT-8
TEST_F(RouterNode, IgnoresStaleActionFrames)
{
  RouterFixture fixture("act_stale", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  bool feedback_seen = false;
  rclcpp_action::Client<tf2_msgs::action::LookupTransform>::SendGoalOptions options;
  options.feedback_callback =
    [&feedback_seen](rclcpp_action::ClientGoalHandle<
      tf2_msgs::action::LookupTransform>::SharedPtr,
    const std::shared_ptr<const tf2_msgs::action::LookupTransform_Feedback> &) {
      feedback_seen = true;
    };
  // Feedback and result before any goal name no live goal either.
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActFeedback, 0x00u, 0x00u, 0x09u});
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActResult, 0x00u, 0x00u, 0x04u, 0x09u});
  auto goal_handle = client->async_send_goal(make_spin_goal(), options);
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 4u, {kHelperActGoalResp, 0x01u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x01u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  const auto handle = goal_handle.get();
  ASSERT_NE(handle, nullptr);

  // A duplicate GOAL_RESP, feedback and result for other seqs, and feedback
  // for an unknown action all report nothing to ROS. The rel numbers stay
  // in order so the session applies every frame; the router drops the ones
  // naming no live goal.
  fixture.deliver_frame(kHelperActions, 5u, {kHelperActGoalResp, 0x01u, 0x00u, 0x00u});
  fixture.deliver_frame(kHelperActions, 6u, {kHelperActFeedback, 0x02u, 0x01u, 0x09u});
  fixture.deliver_frame(kHelperActions, 7u, {kHelperActFeedback, 0x03u, 0x07u, 0x09u});
  fixture.deliver_frame(kHelperActions, 8u, {kHelperActResult, 0x02u, 0x01u, 0x04u, 0x09u});
  const auto start = std::chrono::steady_clock::now();
  while (!feedback_seen && std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  EXPECT_FALSE(feedback_seen) << "stray frames never reach the ROS client";

  // The live goal still completes once its own frames arrive.
  fixture.deliver_frame(kHelperActions, 9u, {kHelperActFeedback, 0x05u, 0x00u});
  auto result_future = client->async_get_result(handle);
  Converter converter;
  tf2_msgs::action::LookupTransform_Result result;
  const auto packed = pack_action(
    "tf2_msgs/action/LookupTransform_Result", &result, converter);
  std::vector<std::uint8_t> frame{kHelperActResult, 0x03u, 0x00u, 0x04u};
  frame.insert(frame.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 10u, frame);
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::SUCCEEDED);

  // Feedback and result after the terminal state name no live goal either.
  fixture.deliver_frame(kHelperActions, 11u, {kHelperActFeedback, 0x00u, 0x00u, 0x09u});
  std::vector<std::uint8_t> late{kHelperActResult, 0x04u, 0x00u, 0x04u};
  late.insert(late.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 12u, late);
  const auto quiet = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - quiet < std::chrono::seconds(2)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  EXPECT_FALSE(feedback_seen) << "terminal goals take no more feedback";
  EXPECT_EQ(fixture.router().diagnostics().link_state, "connected");
}

// covers: SES-35, ACT-6
TEST_F(RouterNode, AbortsTheGoalOnAnUndecodableResult)
{
  RouterFixture fixture("act_badresult", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  ASSERT_NE(goal_handle.get(), nullptr);

  // A result whose payload cannot unpack aborts the goal instead of
  // stranding the ROS client.
  auto result_future = client->async_get_result(goal_handle.get());
  fixture.deliver_frame(kHelperActions, 3u,
    {kHelperActResult, 0x01u, 0x00u, 0x04u, 0xC8u, 0x01u});
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::ABORTED);
  EXPECT_TRUE(logged_has(fixture.logged(), "dropped result"));
}

// covers: SES-35
TEST_F(RouterNode, RejectsTheGoalTheMcuNeverAnswers)
{
  RouterFixture fixture("act_timeout", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  // The manual clock jumps past the call timeout: no MCU answer arrives.
  fixture.advance(6000u);
  rclcpp::spin_some(fixture.node());
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  EXPECT_EQ(goal_handle.get(), nullptr) << "an unanswered goal is refused, not stranded";
  EXPECT_TRUE(logged_has(fixture.logged(), "never answered the goal"));
  // A late GOAL_RESP for the forgotten goal applies to nothing.
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  const auto quiet = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - quiet < std::chrono::seconds(1)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
}

// covers: SES-35, ACT-5
TEST_F(RouterNode, RejectsTheCancelTheMcuNeverAnswers)
{
  RouterFixture fixture("act_canceltimeout", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  ASSERT_NE(goal_handle.get(), nullptr);

  auto cancel = client->async_cancel_goal(goal_handle.get());
  const auto start = std::chrono::steady_clock::now();
  bool cancel_wire = false;
  while (!cancel_wire && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActCancel)
      {
        cancel_wire = true;
      }
    }
  }
  ASSERT_TRUE(cancel_wire);
  // The manual clock jumps past the call timeout: no MCU answer arrives.
  fixture.advance(6000u);
  rclcpp::spin_some(fixture.node());
  ASSERT_TRUE(spin_until_ready(fixture, cancel));
  EXPECT_EQ(cancel.get()->return_code, 1) << "an unanswered cancel reports rejection";
  EXPECT_TRUE(logged_has(fixture.logged(), "never answered the cancel"));
  // A late CANCEL_RESP for the forgotten cancel applies to nothing.
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActCancelResp, 0x01u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x01u});
  const auto quiet = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - quiet < std::chrono::seconds(1)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
}

// covers: SES-35
TEST_F(RouterNode, RejectsCallsWhileTheLinkIsDown)
{
  RouterFixture fixture("act_down", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  // A goal and its cancel go out, then the manual clock jumps past the
  // link timeout: the disconnect aborts both instead of stranding them.
  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  const auto handle = goal_handle.get();
  ASSERT_NE(handle, nullptr);
  auto cancel = client->async_cancel_goal(handle);
  auto result_future = client->async_get_result(handle);
  const auto cancel_start = std::chrono::steady_clock::now();
  bool cancel_wire = false;
  while (!cancel_wire &&
    std::chrono::steady_clock::now() - cancel_start < std::chrono::seconds(10))
  {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActCancel)
      {
        cancel_wire = true;
      }
    }
  }
  ASSERT_TRUE(cancel_wire);

  fixture.advance(3600001u);
  rclcpp::spin_some(fixture.node());
  ASSERT_TRUE(spin_until_ready(fixture, cancel));
  EXPECT_EQ(cancel.get()->return_code, 1) << "the lost cancel reports rejection";
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::ABORTED);

  // A new goal while the link is down is refused at once.
  auto late = client->async_send_goal(make_spin_goal());
  ASSERT_TRUE(spin_until_ready(fixture, late));
  EXPECT_EQ(late.get(), nullptr) << "no ROS goal waits on a dead link";
}

// covers: SES-35
TEST_F(RouterNode, RejectsASecondGoalWhileOneIsPending)
{
  RouterFixture fixture("act_secondpending", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  // The first goal goes out but the MCU never answers it yet.
  auto first = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));

  // A second goal while the first is still pending is rejected at once.
  auto second = client->async_send_goal(make_spin_goal());
  ASSERT_TRUE(spin_until_ready(fixture, second));
  EXPECT_EQ(second.get(), nullptr) << "a pending goal blocks the next one";
  bool second_wire = false;
  for (const auto & unit : fixture.drain()) {
    const auto decoded = fixture.checked_decode(unit);
    if (decoded.size() >= 7u && decoded[0] == kHelperActions &&
      decoded[2] == kHelperActGoal)
    {
      second_wire = true;
    }
  }
  EXPECT_FALSE(second_wire) << "the rejected goal never reaches the wire";

  // The first goal still completes once the MCU accepts it.
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, first));
  EXPECT_NE(first.get(), nullptr);
}

// covers: SES-35, ACT-4
TEST_F(RouterNode, IgnoresAGoalResponseForAnotherSeq)
{
  RouterFixture fixture("act_wrongseq", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  // A GOAL_RESP for another seq applies to no pending goal.
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x09u, 0x00u});
  for (int i = 0; i < 10; ++i) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  EXPECT_FALSE(
    goal_handle.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    << "a wrong-seq response strands nothing, answers nothing";
  // The live goal still completes on its own response.
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActGoalResp, 0x01u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x01u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  EXPECT_NE(goal_handle.get(), nullptr);
}

// covers: SES-35
TEST_F(RouterNode, DropsFeedbackForNoLiveGoal)
{
  RouterFixture fixture("act_badfb", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  bool feedback_seen = false;
  rclcpp_action::Client<tf2_msgs::action::LookupTransform>::SendGoalOptions options;
  options.feedback_callback =
    [&feedback_seen](rclcpp_action::ClientGoalHandle<
      tf2_msgs::action::LookupTransform>::SharedPtr,
    const std::shared_ptr<const tf2_msgs::action::LookupTransform_Feedback> &) {
      feedback_seen = true;
    };
  // Feedback for a seq with no live goal is dropped before parsing: the
  // route exists but nothing is active on it.
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActFeedback, 0xFFu, 0x00u});
  for (int i = 0; i < 10; ++i) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  EXPECT_FALSE(feedback_seen) << "a bad-seq feedback publishes nothing";

  // With a live goal, feedback for another seq is dropped the same way:
  // the active seq does not match.
  auto goal_handle = client->async_send_goal(make_spin_goal(), options);
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActGoalResp, 0x01u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x01u});
  for (int i = 0; i < 10; ++i) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  fixture.deliver_frame(kHelperActions, 4u, {kHelperActFeedback, 0xFFu, 0x00u});
  for (int i = 0; i < 10; ++i) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  EXPECT_FALSE(feedback_seen) << "a mismatched-seq feedback publishes nothing";
}

// covers: SES-35
TEST_F(RouterNode, DropsResultForNoLiveGoal)
{
  RouterFixture fixture("act_badres", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  Converter converter;
  tf2_msgs::action::LookupTransform_Result result;
  const auto packed = pack_action(
    "tf2_msgs/action/LookupTransform_Result", &result, converter);
  // A result for a seq with no live goal is dropped: the route exists but
  // nothing is active on it.
  std::vector<std::uint8_t> bad_frame{kHelperActResult, 0x00u, 0xFFu, 0x04u};
  bad_frame.insert(bad_frame.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 2u, bad_frame);
  for (int i = 0; i < 10; ++i) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  // The bridge stays connected and the action server still answers.
  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActGoalResp, 0x01u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x01u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  EXPECT_NE(goal_handle.get(), nullptr);
}

// covers: SES-35, REL-2
TEST_F(RouterNode, DropsAnUnackedGoalAfterThreeRetries)
{
  RouterFixture fixture("act_goaldrop", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  // Never ack the GOAL: three 500 ms retries pass, then the frame drops.
  // Each pump frame drives the session poll past the next retry deadline.
  for (int i = 0; i < 4; ++i) {
    fixture.advance(500u);
    rclcpp::spin_some(fixture.node());
    fixture.deliver_frame(
      kAckTopic, static_cast<std::uint8_t>(i), {kHelperActions, 0x7Fu});
  }
  EXPECT_TRUE(logged_has(fixture.logged(), "no ack for the action frame"))
    << "the unacked GOAL is dropped and counted";
  // A late GOAL_RESP names a seq nothing tracks: it is ignored as stale.
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, goal_seq, 0x00u});
  for (int i = 0; i < 5; ++i) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  EXPECT_FALSE(goal_handle.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    << "the dropped goal strands nothing and answers nothing";
}

// covers: SES-35, REL-2
TEST_F(RouterNode, DropsAnUnackedCancelAfterThreeRetries)
{
  RouterFixture fixture("act_canceldrop", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  ASSERT_NE(goal_handle.get(), nullptr);

  auto cancel = client->async_cancel_goal(goal_handle.get());
  bool cancel_wire = false;
  const auto start = std::chrono::steady_clock::now();
  while (!cancel_wire && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActCancel)
      {
        cancel_wire = true;
      }
    }
  }
  ASSERT_TRUE(cancel_wire) << "the CANCEL never reached the wire";
  // Never ack the CANCEL: three retries pass, then it drops.
  for (int i = 0; i < 4; ++i) {
    fixture.advance(500u);
    rclcpp::spin_some(fixture.node());
    fixture.deliver_frame(
      kAckTopic, static_cast<std::uint8_t>(i), {kHelperActions, 0x7Fu});
  }
  EXPECT_TRUE(logged_has(fixture.logged(), "no ack for the action frame"))
    << "the unacked CANCEL is dropped and counted";
}

// covers: SES-35
TEST_F(RouterNode, RejectsTheCancelOfATerminalGoal)
{
  RouterFixture fixture("act_termcancel", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  const auto handle = goal_handle.get();
  ASSERT_NE(handle, nullptr);

  Converter converter;
  tf2_msgs::action::LookupTransform_Result result;
  const auto packed = pack_action(
    "tf2_msgs/action/LookupTransform_Result", &result, converter);
  std::vector<std::uint8_t> frame{kHelperActResult, 0x01u, 0x00u, 0x04u};
  frame.insert(frame.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 4u, frame);

  // Canceling a terminal goal reports it instead of touching the wire. The
  // cancel goes out before the client first spins on the result, so the
  // client still holds the handle while the bridge already stored it.
  auto cancel = client->async_cancel_goal(handle);
  ASSERT_TRUE(spin_until_ready(fixture, cancel));
  EXPECT_EQ(cancel.get()->return_code, 3) << "a terminal goal reports its end";
  auto result_future = client->async_get_result(handle);
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::SUCCEEDED);
  bool cancel_wire = false;
  const auto start = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(500)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActCancel)
      {
        cancel_wire = true;
      }
    }
  }
  EXPECT_FALSE(cancel_wire) << "no CANCEL goes out for a terminal goal";
}

// covers: SES-35
TEST_F(RouterNode, ResyncsIdleRoutes)
{
  RouterFixture fixture("act_idle", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  // No goal ever runs here: the re-handshake syncs routes with no active
  // goal to abort and no status to publish.
  fixture.script_frame(kHelperControl, 5u, helper_hello(0x4445u, 1u, 512u, 0xCCu));
  fixture.script_frame(kHelperControl, 6u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.script_frame(kParamsTopic, 1u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  fixture.script_frame(kServicesTopic, 1u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> hash =
    installed_action_hash("tf2_msgs/action/LookupTransform");
  fixture.script_frame(kHelperActions, 5u,
    helper_action_declare(0u, hash.data(), "tf2_msgs/action/LookupTransform", "spin"));
  fixture.script_frame(kHelperActions, 6u,
    std::vector<std::uint8_t>{kHelperActStatus, 0x00u, 0x01u, 0x01u});
  fixture.router().poll();
  EXPECT_EQ(fixture.router().diagnostics().link_state, "connected")
    << "the re-handshake completes with no goals around";
}

// covers: SES-35, ACT-6
TEST_F(RouterNode, CompletesTheGoalOnAnAbortedOutcome)
{
  RouterFixture fixture("act_abortresult", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  ASSERT_NE(goal_handle.get(), nullptr);
  auto result_future = client->async_get_result(goal_handle.get());

  // The MCU aborts the goal itself: the terminal outcome completes the ROS
  // goal as aborted.
  Converter converter;
  tf2_msgs::action::LookupTransform_Result result;
  const auto packed = pack_action(
    "tf2_msgs/action/LookupTransform_Result", &result, converter);
  std::vector<std::uint8_t> frame{kHelperActResult, 0x01u, 0x00u, 0x06u};
  frame.insert(frame.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 3u, frame);
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::ABORTED);
}

// covers: SES-35
TEST_F(RouterNode, AbortsThePendingGoalOnDisconnect)
{
  RouterFixture fixture("act_pendabort", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  // The goal goes out but the MCU never answers it; the link dies first.
  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.advance(3600001u);
  rclcpp::spin_some(fixture.node());
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  EXPECT_EQ(goal_handle.get(), nullptr) << "a pending goal is refused on disconnect";
}

// covers: SES-35, ACT-5
TEST_F(RouterNode, RefusesTheCancelTheMcuRefuses)
{
  RouterFixture fixture("act_cancelrefuse", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  ASSERT_NE(goal_handle.get(), nullptr);

  auto cancel = client->async_cancel_goal(goal_handle.get());
  const auto start = std::chrono::steady_clock::now();
  bool cancel_wire = false;
  while (!cancel_wire && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActCancel)
      {
        cancel_wire = true;
      }
    }
  }
  ASSERT_TRUE(cancel_wire) << "the CANCEL never reached the wire";
  // The MCU refuses the cancel: the ROS cancel reports the refusal and the
  // goal stays live.
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActCancelResp, 0x01u, 0x00u, 0x01u});
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x01u});
  ASSERT_TRUE(spin_until_ready(fixture, cancel));
  EXPECT_EQ(cancel.get()->return_code, 1) << "a refused cancel reports rejection";

  // The refused cancel leaves the goal live: its result still completes it.
  auto result_future = client->async_get_result(goal_handle.get());
  Converter converter;
  tf2_msgs::action::LookupTransform_Result result;
  const auto packed = pack_action(
    "tf2_msgs/action/LookupTransform_Result", &result, converter);
  std::vector<std::uint8_t> frame{kHelperActResult, 0x02u, 0x00u, 0x04u};
  frame.insert(frame.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 4u, frame);
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::SUCCEEDED);
}

// covers: SES-35, ACT-3
TEST_F(RouterNode, AbortsTheGoalOnAnUndecodableFeedback)
{
  RouterFixture fixture("act_badfeedback", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  bool feedback_seen = false;
  rclcpp_action::Client<tf2_msgs::action::LookupTransform>::SendGoalOptions options;
  options.feedback_callback =
    [&feedback_seen](rclcpp_action::ClientGoalHandle<
      tf2_msgs::action::LookupTransform>::SharedPtr,
    const std::shared_ptr<const tf2_msgs::action::LookupTransform_Feedback> &) {
      feedback_seen = true;
    };
  auto goal_handle = client->async_send_goal(make_spin_goal(), options);
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  ASSERT_NE(goal_handle.get(), nullptr);

  // Feedback that cannot unpack is dropped and never reaches ROS: the
  // frame carries no rel_seq, only the goal seq, the index and the payload.
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActFeedback, 0x00u, 0x00u, 0xC8u});
  const auto start = std::chrono::steady_clock::now();
  while (!feedback_seen && std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
  }
  EXPECT_FALSE(feedback_seen) << "undecodable feedback never reaches the ROS client";
  EXPECT_TRUE(logged_has(fixture.logged(), "dropped feedback"));

  // The live goal still completes once its own result arrives.
  auto result_future = client->async_get_result(goal_handle.get());
  Converter converter;
  tf2_msgs::action::LookupTransform_Result result;
  const auto packed = pack_action(
    "tf2_msgs/action/LookupTransform_Result", &result, converter);
  std::vector<std::uint8_t> frame{kHelperActResult, 0x01u, 0x00u, 0x04u};
  frame.insert(frame.end(), packed.begin(), packed.end());
  fixture.deliver_frame(kHelperActions, 4u, frame);
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::SUCCEEDED);
}

// covers: SES-35
TEST_F(RouterNode, PublishesTheAbortedStatusOnResync)
{
  RouterFixture fixture("act_resync", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  ASSERT_NE(goal_handle.get(), nullptr);
  auto result_future = client->async_get_result(goal_handle.get());

  std::vector<action_msgs::msg::GoalStatusArray> statuses;
  auto sub = fixture.node()->create_subscription<action_msgs::msg::GoalStatusArray>(
    "spin/_action/status", 10,
    [&statuses](action_msgs::msg::GoalStatusArray::SharedPtr msg) {
      statuses.push_back(*msg);
    });

  // Queue the whole re-handshake without polling: the abort and the
  // re-handshake land in one router poll, so the sync itself publishes the
  // aborted status before any periodic publish can clear it.
  const std::array<std::uint8_t, 8> hash =
    installed_action_hash("tf2_msgs/action/LookupTransform");
  fixture.script_frame(kHelperControl, 5u, helper_hello(0x4445u, 1u, 512u, 0xCCu));
  fixture.script_frame(kHelperControl, 6u,
    helper_topic(0u, kBattery, 0x00u, installed_hash("std_msgs/msg/Float32").data(),
      "std_msgs/msg/Float32", "battery"));
  fixture.script_frame(kParamsTopic, 1u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  fixture.script_frame(kServicesTopic, 1u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  fixture.script_frame(kHelperActions, 5u,
    helper_action_declare(0u, hash.data(), "tf2_msgs/action/LookupTransform", "spin"));
  fixture.script_frame(kHelperActions, 6u,
    std::vector<std::uint8_t>{kHelperActStatus, 0x00u, 0x01u, 0x01u});
  fixture.router().poll();
  rclcpp::spin_some(fixture.node());
  EXPECT_EQ(fixture.router().diagnostics().link_state, "connected")
    << "the re-handshake completed in one poll";
  ASSERT_TRUE(spin_until_ready(fixture, result_future));
  EXPECT_EQ(result_future.get().code, rclcpp_action::ResultCode::ABORTED);
  bool aborted_seen = false;
  for (const auto & status : statuses) {
    for (const auto & entry : status.status_list) {
      if (entry.status == action_msgs::msg::GoalStatus::STATUS_ABORTED) {
        aborted_seen = true;
      }
    }
  }
  EXPECT_TRUE(aborted_seen) << "the sync published the aborted goal status";
}

// covers: SES-35
TEST_F(RouterNode, PipelinesAGoalBehindAnUnackedOne)
{
  RouterFixture fixture("act_pipe", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_two_actions(fixture);
  auto first_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  auto second_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin2");
  ASSERT_TRUE(first_client->wait_for_action_server(std::chrono::seconds(10)));
  ASSERT_TRUE(second_client->wait_for_action_server(std::chrono::seconds(10)));

  // The first goal goes out unacked; the second goal queues behind it and
  // takes the next free wire seq instead of clashing with the live one.
  auto first = first_client->async_send_goal(make_spin_goal());
  std::uint8_t first_index = 0xFFu;
  std::uint8_t first_seq = 0xFFu;
  ASSERT_TRUE(wait_for_indexed_goal(fixture, first_index, first_seq));
  auto second = second_client->async_send_goal(make_spin_goal());
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x00u});
  std::uint8_t second_index = 0xFFu;
  std::uint8_t second_seq = 0xFFu;
  const auto start = std::chrono::steady_clock::now();
  while (second_index != 0x01u &&
    std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
  {
    if (!wait_for_indexed_goal(fixture, second_index, second_seq)) {
      break;
    }
  }
  EXPECT_EQ(second_index, 0x01u) << "the second action walks its own wire index";
  EXPECT_EQ(second_seq, 0x01u) << "the bridge skips seqs live on other actions";
  fixture.deliver_frame(kAckTopic, 4u, {kHelperActions, 0x01u});
  fixture.deliver_frame(kHelperActions, 5u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kHelperActions, 6u, {kHelperActGoalResp, 0x01u, 0x01u, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, first));
  ASSERT_TRUE(spin_until_ready(fixture, second));
  EXPECT_NE(first.get(), nullptr);
  EXPECT_NE(second.get(), nullptr);
}

// covers: SES-35
TEST_F(RouterNode, AnswersTheSecondRouteWhileTheFirstStillPends)
{
  RouterFixture fixture("act_secondfirst", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_two_actions(fixture);
  auto first_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  auto second_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin2");
  ASSERT_TRUE(first_client->wait_for_action_server(std::chrono::seconds(10)));
  ASSERT_TRUE(second_client->wait_for_action_server(std::chrono::seconds(10)));

  // Both goals go out and stay pending: the bridge skips seqs live on the
  // other action, so the two wire seqs differ.
  auto first = first_client->async_send_goal(make_spin_goal());
  std::uint8_t first_index = 0xFFu;
  std::uint8_t first_seq = 0xFFu;
  ASSERT_TRUE(wait_for_indexed_goal(fixture, first_index, first_seq));
  auto second = second_client->async_send_goal(make_spin_goal());
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x00u});
  std::uint8_t second_index = 0xFFu;
  std::uint8_t second_seq = 0xFFu;
  const auto start = std::chrono::steady_clock::now();
  while (second_index != 0x01u &&
    std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
  {
    if (!wait_for_indexed_goal(fixture, second_index, second_seq)) {
      break;
    }
  }
  ASSERT_EQ(second_index, 0x01u);
  ASSERT_NE(first_seq, second_seq);

  // The MCU answers the second route first: the router walks past the first
  // route's pending goal to reach the second one.
  fixture.deliver_frame(kAckTopic, 4u, {kHelperActions, 0x01u});
  fixture.deliver_frame(kHelperActions, 5u, {kHelperActGoalResp, 0x00u, second_seq, 0x00u});
  fixture.deliver_frame(kHelperActions, 6u, {kHelperActGoalResp, 0x01u, first_seq, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, first));
  ASSERT_TRUE(spin_until_ready(fixture, second));
  EXPECT_NE(first.get(), nullptr);
  EXPECT_NE(second.get(), nullptr);
}

// covers: SES-35
TEST_F(RouterNode, AcceptsAGoalWhileAnotherActionRuns)
{
  RouterFixture fixture("act_tworun", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_two_actions(fixture);
  auto first_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  auto second_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin2");
  ASSERT_TRUE(first_client->wait_for_action_server(std::chrono::seconds(10)));
  ASSERT_TRUE(second_client->wait_for_action_server(std::chrono::seconds(10)));

  // Accept the first goal, then start the second: the bridge skips the live
  // wire seq of the other action instead of clashing with it.
  auto first = first_client->async_send_goal(make_spin_goal());
  std::uint8_t first_index = 0xFFu;
  std::uint8_t first_seq = 0xFFu;
  ASSERT_TRUE(wait_for_indexed_goal(fixture, first_index, first_seq));
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x00u});
  fixture.deliver_frame(kHelperActions, 4u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, first));
  ASSERT_NE(first.get(), nullptr);
  auto second = second_client->async_send_goal(make_spin_goal());
  std::uint8_t second_index = 0xFFu;
  std::uint8_t second_seq = 0xFFu;
  const auto start = std::chrono::steady_clock::now();
  while (second_index != 0x01u &&
    std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
  {
    if (!wait_for_indexed_goal(fixture, second_index, second_seq)) {
      break;
    }
  }
  EXPECT_EQ(second_index, 0x01u);
  EXPECT_EQ(second_seq, 0x01u) << "the bridge skips the live seq of the other action";
  fixture.deliver_frame(kAckTopic, 5u, {kHelperActions, 0x01u});
  fixture.deliver_frame(kHelperActions, 6u, {kHelperActGoalResp, 0x01u, 0x01u, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, second));
  EXPECT_NE(second.get(), nullptr);
}

// covers: SES-35
TEST_F(RouterNode, AcceptsAGoalWhileAnotherIsActive)
{
  RouterFixture fixture("act_tworun", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_two_actions(fixture);
  auto first_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  auto second_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin2");
  ASSERT_TRUE(first_client->wait_for_action_server(std::chrono::seconds(10)));
  ASSERT_TRUE(second_client->wait_for_action_server(std::chrono::seconds(10)));

  // Accept the first goal, then start the second: the bridge skips the live
  // wire seq of the other action instead of clashing with it.
  auto first = first_client->async_send_goal(make_spin_goal());
  std::uint8_t first_index = 0xFFu;
  std::uint8_t first_seq = 0xFFu;
  ASSERT_TRUE(wait_for_indexed_goal(fixture, first_index, first_seq));
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x00u});
  fixture.deliver_frame(kHelperActions, 5u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, first));
  ASSERT_NE(first.get(), nullptr);
  auto second = second_client->async_send_goal(make_spin_goal());
  std::uint8_t second_index = 0xFFu;
  std::uint8_t second_seq = 0xFFu;
  const auto start = std::chrono::steady_clock::now();
  while (second_index != 0x01u &&
    std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
  {
    if (!wait_for_indexed_goal(fixture, second_index, second_seq)) {
      break;
    }
  }
  EXPECT_EQ(second_index, 0x01u);
  EXPECT_EQ(second_seq, 0x01u) << "the bridge skips the live seq of the other action";
  fixture.deliver_frame(kAckTopic, 4u, {kHelperActions, 0x01u});
  fixture.deliver_frame(kHelperActions, 6u, {kHelperActGoalResp, 0x01u, 0x01u, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, second));
  EXPECT_NE(second.get(), nullptr);
}

// covers: SES-35, ACT-5
TEST_F(RouterNode, CancelsOneOfTwoLiveGoals)
{
  RouterFixture fixture("act_two_cancel", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_two_actions(fixture);
  auto first_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  auto second_client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin2");
  ASSERT_TRUE(first_client->wait_for_action_server(std::chrono::seconds(10)));
  ASSERT_TRUE(second_client->wait_for_action_server(std::chrono::seconds(10)));

  auto first = first_client->async_send_goal(make_spin_goal());
  std::uint8_t first_index = 0xFFu;
  std::uint8_t first_seq = 0xFFu;
  ASSERT_TRUE(wait_for_indexed_goal(fixture, first_index, first_seq));
  fixture.deliver_frame(kAckTopic, 3u, {kHelperActions, 0x00u});
  auto second = second_client->async_send_goal(make_spin_goal());
  std::uint8_t second_index = 0xFFu;
  std::uint8_t second_seq = 0xFFu;
  ASSERT_TRUE(wait_for_indexed_goal(fixture, second_index, second_seq));
  fixture.deliver_frame(kAckTopic, 4u, {kHelperActions, 0x01u});
  fixture.deliver_frame(kHelperActions, 5u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kHelperActions, 6u, {kHelperActGoalResp, 0x01u, 0x01u, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, first));
  ASSERT_TRUE(spin_until_ready(fixture, second));
  ASSERT_NE(first.get(), nullptr);
  ASSERT_NE(second.get(), nullptr);

  // Canceling the second goal answers on its own route; the first route
  // comes first in the map and sees a cancel response for no pending
  // cancel, so it moves on.
  auto cancel = second_client->async_cancel_goal(second.get());
  const auto start = std::chrono::steady_clock::now();
  bool cancel_wire = false;
  while (!cancel_wire && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
    fixture.advance(20u);
    rclcpp::spin_some(fixture.node());
    for (const auto & unit : fixture.drain()) {
      const auto decoded = fixture.checked_decode(unit);
      if (decoded.size() >= 6u && decoded[0] == kHelperActions &&
        decoded[2] == kHelperActCancel && decoded[4] == 0x01u)
      {
        cancel_wire = true;
      }
    }
  }
  ASSERT_TRUE(cancel_wire) << "the CANCEL never reached the wire";
  fixture.deliver_frame(kHelperActions, 7u, {kHelperActCancelResp, 0x02u, 0x01u, 0x00u});
  fixture.deliver_frame(kAckTopic, 7u, {kHelperActions, 0x02u});
  ASSERT_TRUE(spin_until_ready(fixture, cancel));
  EXPECT_EQ(cancel.get()->return_code, 0) << "the MCU accepted the cancel";
}

// covers: SES-35, ACT-5
TEST_F(RouterNode, RefusesTheCancelTheLinkRefuses)
{
  RouterFixture fixture("act_downcancel", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  auto goal_handle = client->async_send_goal(make_spin_goal());
  std::uint8_t goal_seq = 0xFFu;
  std::vector<std::uint8_t> payload;
  ASSERT_TRUE(wait_for_wire_goal(fixture, goal_seq, payload));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAckTopic, 2u, {kHelperActions, 0x00u});
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  ASSERT_NE(goal_handle.get(), nullptr);

  // The cancel goes out just as the link dies: the take still sees the live
  // goal, but the session refuses the CANCEL, so the ROS cancel is rejected
  // instead of queuing behind the dead link.
  auto cancel = client->async_cancel_goal(goal_handle.get());
  fixture.advance(3600001u);
  rclcpp::spin_some(fixture.node());
  ASSERT_TRUE(spin_until_ready(fixture, cancel));
  EXPECT_EQ(cancel.get()->return_code, 1) << "a cancel on a dead link reports rejection";
  EXPECT_TRUE(logged_has(fixture.logged(), "the link refused the ROS cancel"));
}

// covers: SES-35, ACT-3
TEST_F(RouterNode, DropsTheGoalThatDoesNotPack)
{
  RouterFixture fixture("act_badgoal", remapped(), RouterParams{},
    512u, true, action_session_config());
  handshake_with_actions(fixture);
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(
    fixture.node(), "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));

  // A frame name past the wire length passes the middleware but never
  // packs, so the take drops the goal straight away.
  auto big = make_spin_goal();
  big.target_frame = std::string(70000u, 'f');
  auto goal_handle = client->async_send_goal(big);
  ASSERT_TRUE(spin_until_ready(fixture, goal_handle));
  EXPECT_EQ(goal_handle.get(), nullptr) << "an unpackable goal is refused, not stranded";
  EXPECT_TRUE(logged_has(fixture.logged(), "dropped ROS goal"));
  bool goal_wire = false;
  for (const auto & unit : fixture.drain()) {
    const auto decoded = fixture.checked_decode(unit);
    if (decoded.size() >= 7u && decoded[0] == kHelperActions &&
      decoded[2] == kHelperActGoal)
    {
      goal_wire = true;
    }
  }
  EXPECT_FALSE(goal_wire) << "the unpackable goal never reaches the wire";
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
