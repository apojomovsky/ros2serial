// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The action waker: a raw rcl action server stands in for a route, and each
// test drives the waker by hand instead of an executor, so what it asserts
// is the wakeup itself rather than a take a poll loop could have produced.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <rcl_action/action_server.h>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <tf2_msgs/action/lookup_transform.hpp>

#include "r2s_bridge/action_types.hpp"
#include "r2s_bridge/action_waker.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

// One raw rcl action server on the node, the way a route holds it: the
// waker watches the handle, never the route.
class RawServer
{
public:
  RawServer(rclcpp::Node::SharedPtr node, const std::string & name)
  : node_(std::move(node)), types_("tf2_msgs/action/LookupTransform")
  {
    rcl_node_t * node_handle = node_->get_node_base_interface()->get_shared_rcl_node_handle().get();
    rcl_clock_t * clock = node_->get_clock()->get_clock_handle();
    const rcl_action_server_options_t options = rcl_action_server_get_default_options();
    if (rcl_action_server_init(
          &server_, node_handle, clock, types_.cpp_action_handle(), name.c_str(), &options) !=
      RCL_RET_OK)
    {
      rcl_reset_error();
      throw std::runtime_error("cannot init the raw action server");
    }
  }
  ~RawServer()
  {
    rcl_node_t * node_handle = node_->get_node_base_interface()->get_shared_rcl_node_handle().get();
    if (rcl_action_server_fini(&server_, node_handle) != RCL_RET_OK) {
      rcl_reset_error();
    }
  }
  RawServer(const RawServer &) = delete;
  RawServer & operator=(const RawServer &) = delete;

  const rcl_action_server_t * handle() const {return &server_;}

  // Takes one pending goal, draining the readiness the test staged: the
  // transmit path takes for real, so the test takes the same way.
  bool take_goal()
  {
    auto take = types_.make_goal_request();
    rmw_request_id_t header{};
    const rcl_ret_t taken = rcl_action_take_goal_request(&server_, &header, take.storage.data());
    if (taken != RCL_RET_OK) {
      rcl_reset_error();
    }
    return taken == RCL_RET_OK;
  }

private:
  rclcpp::Node::SharedPtr node_;
  ActionTypes types_;
  rcl_action_server_t server_ = rcl_action_get_zero_initialized_server();
};

class ActionWakerTest : public ::testing::Test
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

  void SetUp() override
  {
    node_ = std::make_shared<rclcpp::Node>("waker_probe");
  }
  void TearDown() override
  {
    node_.reset();
  }

  // Spins until the waker fires, or the wait expires: the client needs a
  // turn for discovery before its goal reaches the server.
  static bool spin_until_triggered(
    rclcpp::Node::SharedPtr node, const std::atomic<bool> & fired)
  {
    const auto start = std::chrono::steady_clock::now();
    while (!fired.load() && std::chrono::steady_clock::now() - start < std::chrono::seconds(10)) {
      rclcpp::spin_some(node);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return fired.load();
  }

  rclcpp::Node::SharedPtr node_;
};

// A goal request wakes the waiter: the take it announces runs on the
// executor's transmit, not on the watcher thread.
TEST_F(ActionWakerTest, WakesWhenAGoalArrives)
{
  RawServer server(node_, "spin");
  std::atomic<bool> fired{false};
  ActionWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
    fired.store(true);
  });
  waker.watch({server.handle()});
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(node_, "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));
  tf2_msgs::action::LookupTransform_Goal goal;
  goal.target_frame = "base";
  ASSERT_TRUE(spin_until_triggered(node_, fired)) << "a goal must wake the waiter";
  waker.unwatch();
}

// No servers means no wakeups: the waiter parks instead of polling.
TEST_F(ActionWakerTest, StaysQuietWithNoServers)
{
  std::atomic<bool> fired{false};
  ActionWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
    fired.store(true);
  });
  waker.watch({});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(fired.load()) << "an empty watch must not wake";
  waker.unwatch();
}

// A watched server with no traffic stays quiet: the waiter blocks in the
// wait instead of spinning on an idle set.
TEST_F(ActionWakerTest, StaysQuietOnAnIdleServer)
{
  RawServer server(node_, "spin");
  std::atomic<bool> fired{false};
  ActionWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
    fired.store(true);
  });
  waker.watch({server.handle()});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(fired.load()) << "an idle server must not wake";
  waker.unwatch();
}

// Unwatching drops the set: a goal that arrives after the drop wakes
// nothing, even though its entity is still ready.
TEST_F(ActionWakerTest, UnwatchStopsTheWakeups)
{
  RawServer server(node_, "spin");
  std::atomic<bool> fired{false};
  ActionWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
    fired.store(true);
  });
  waker.watch({server.handle()});
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(node_, "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));
  tf2_msgs::action::LookupTransform_Goal goal;
  goal.target_frame = "base";
  client->async_send_goal(goal);
  ASSERT_TRUE(spin_until_triggered(node_, fired)) << "a goal must wake the waiter";
  // Nothing was taken, so the waiter parks on its edge latch: the drop
  // below only needs to stop the wakeups, not to drain anything.
  waker.notify_served(false);
  waker.unwatch();
  fired.store(false);
  client->async_send_goal(goal);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_FALSE(fired.load()) << "a dropped set must not wake";
}

// Watching a new set drops the old one: a goal on the dropped server is
// invisible, and a goal on the watched one still wakes.
TEST_F(ActionWakerTest, WatchSwapsTheWatchedSet)
{
  RawServer first(node_, "spin");
  RawServer second(node_, "spin2");
  std::atomic<int> fires{0};
  ActionWaker waker(node_->get_node_base_interface()->get_context(), [&fires]() {
    ++fires;
  });
  auto first_client =
    rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(node_, "spin");
  auto second_client =
    rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(node_, "spin2");
  ASSERT_TRUE(first_client->wait_for_action_server(std::chrono::seconds(10)));
  ASSERT_TRUE(second_client->wait_for_action_server(std::chrono::seconds(10)));
  tf2_msgs::action::LookupTransform_Goal goal;
  goal.target_frame = "base";
  waker.watch({first.handle()});
  first_client->async_send_goal(goal);
  const auto start = std::chrono::steady_clock::now();
  while (fires.load() < 1 &&
    std::chrono::steady_clock::now() - start < std::chrono::seconds(10))
  {
    rclcpp::spin_some(node_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(fires.load(), 1) << "a goal on the watched server must wake once";
  // Drain before the serve, the way the transmit path takes before it
  // serves: the re-arm below must see an idle server and not a leftover.
  ASSERT_TRUE(first.take_goal());
  waker.notify_served(true);
  waker.watch({second.handle()});
  second_client->async_send_goal(goal);
  const auto swap = std::chrono::steady_clock::now();
  while (fires.load() < 2 &&
    std::chrono::steady_clock::now() - swap < std::chrono::seconds(10))
  {
    rclcpp::spin_some(node_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(fires.load(), 2) << "a goal on the new server must wake";
  first_client->async_send_goal(goal);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(fires.load(), 2) << "a goal on the dropped server must not wake";
  waker.unwatch();
}

// A served wakeup re-arms: the next goal on the same server wakes again.
TEST_F(ActionWakerTest, ServedRearmsTheWaiter)
{
  RawServer server(node_, "spin");
  std::atomic<int> fires{0};
  ActionWaker waker(node_->get_node_base_interface()->get_context(), [&fires]() {
    ++fires;
  });
  waker.watch({server.handle()});
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(node_, "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));
  tf2_msgs::action::LookupTransform_Goal goal;
  goal.target_frame = "base";
  client->async_send_goal(goal);
  const auto first = std::chrono::steady_clock::now();
  while (fires.load() < 1 &&
    std::chrono::steady_clock::now() - first < std::chrono::seconds(10))
  {
    rclcpp::spin_some(node_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(fires.load(), 1);
  ASSERT_TRUE(server.take_goal());
  waker.notify_served(true);
  client->async_send_goal(goal);
  const auto second = std::chrono::steady_clock::now();
  while (fires.load() < 2 &&
    std::chrono::steady_clock::now() - second < std::chrono::seconds(10))
  {
    rclcpp::spin_some(node_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(fires.load(), 2) << "a second goal must wake again after the serve";
  waker.unwatch();
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
