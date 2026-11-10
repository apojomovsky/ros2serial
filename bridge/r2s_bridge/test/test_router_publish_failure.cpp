// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// A failed serialized publish never aborts the bridge: with the ROS context
// shut down (as in teardown, issue #330) the publish throws, and the router
// logs the topic, counts the failure in diagnostics, and keeps polling.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float32.hpp>

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
constexpr std::uint8_t kPower = 17u;

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

bool logged_has(const std::vector<std::string> & logged, const std::string & part)
{
  return std::any_of(logged.begin(), logged.end(),
           [&part](const std::string & line) {return line.find(part) != std::string::npos;});
}

// Restores the ROS context when the test shuts it down to inject the publish
// failure, so the fixture and the suite tear down on a live context.
class ContextRestore
{
public:
  ~ContextRestore()
  {
    if (!rclcpp::ok()) {
      rclcpp::init(0, nullptr);
    }
  }
};

class PublishFailureFixture
{
public:
  PublishFailureFixture()
  : node_(std::make_shared<rclcpp::Node>("pubfail", "robot1")),
    router_(std::make_unique<Router>(
      node_->get_node_topics_interface(),
        [this]() {return std::unique_ptr<Transport>(new HelperLink(transport_));},
      clock_,
      clock_,
      RouterParams{},
        [this](SessionLog, const std::string & text) {
          logged_.push_back(text);
      },
      SessionConfig{}))
  {
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

    // Answers what the session asks until CONNECTED: two Float32 publishers.
  void handshake()
  {
    deliver_frame(kHelperControl, 0u, helper_hello(7u, 2u));
    expect_topic_req(0u);
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    deliver_frame(
            kHelperControl, 1u,
            helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32",
            "battery"));
    expect_topic_req(1u);
    deliver_frame(kHelperControl, 2u,
                      helper_topic(1u, kPower, 0x00u, float_hash.data(), "std_msgs/msg/Float32",
            "power"));
    drain();
    EXPECT_EQ(router_->publisher_count(), 2u);
  }

  void expect_topic_req(std::uint8_t index)
  {
    const std::vector<std::vector<std::uint8_t>> units = drain();
    ASSERT_FALSE(units.empty());
    const std::vector<std::uint8_t> decoded = helper_decode_unit(units.back());
    ASSERT_GE(decoded.size(), 4u);
    EXPECT_EQ(decoded[0], kHelperControl);
    EXPECT_EQ(decoded[2], 0x03u) << "a TOPIC_REQ";
    EXPECT_EQ(decoded[3], index);
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
  const std::vector<std::string> & logged() const
  {
    return logged_;
  }

private:
  FakeTransport transport_;
  ManualClock clock_;
  rclcpp::Node::SharedPtr node_;
  std::unique_ptr<Router> router_;
  std::vector<std::string> logged_;
};

class RouterPublishFailure : public ::testing::Test
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

TEST_F(RouterPublishFailure, FailedPublishIsLoggedCountedAndSurvived)
{
    ContextRestore restore;
    PublishFailureFixture fixture;
    fixture.handshake();
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "battery", rclcpp::QoS(10).best_effort(),
    [&received](const std_msgs::msg::Float32 & msg) {received.push_back(msg.data);});
    fixture.deliver_frame(kBattery, 0u, {0x00, 0x00, 0xC0, 0x3F});
    rclcpp::spin_some(fixture.node());
    ASSERT_EQ(received.size(), 1u) << "the route publishes while the context is alive";
    // The teardown race of issue #330: the context goes down while the poll
    // loop still delivers frames, so the serialized publish throws.
    rclcpp::shutdown();
    fixture.deliver_frame(kBattery, 1u, {0x00, 0x00, 0xC0, 0x3F});
    fixture.deliver_frame(kPower, 0u, {0x00, 0x00, 0xC0, 0x3F});
    fixture.deliver_frame(kBattery, 2u, {0x00, 0x00, 0xC0, 0x3F});
    EXPECT_TRUE(logged_has(fixture.logged(), "battery")) << "the failure names the topic";
    EXPECT_TRUE(logged_has(fixture.logged(), "power")) << "each topic names itself";
    const Router::Diagnostics diagnostics = fixture.router().diagnostics();
    ASSERT_EQ(diagnostics.topics.size(), 2u);
    ASSERT_EQ(diagnostics.topics[0].topic_id, kBattery);
    ASSERT_EQ(diagnostics.topics[1].topic_id, kPower);
    EXPECT_EQ(diagnostics.topics[0].publish_failures, 2u) << "each failure counts on its own topic";
    EXPECT_EQ(diagnostics.topics[1].publish_failures, 1u);
    EXPECT_EQ(received.size(), 1u) << "nothing publishes after the shutdown";
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
