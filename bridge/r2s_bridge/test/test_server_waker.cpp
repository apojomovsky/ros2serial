// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The server waker: raw rcl servers stand in for routes, and each test
// drives the waker by hand instead of an executor, so what it asserts is
// the wakeup itself rather than a take a poll loop could have produced.
#include <gtest/gtest.h>

#include <rcl/service.h>
#include <rcl_action/action_server.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <rclcpp/rclcpp.hpp>
#include <rclcpp_action/rclcpp_action.hpp>
#include <std_srvs/srv/trigger.hpp>
#include <tf2_msgs/action/lookup_transform.hpp>

#include "r2s_bridge/action_types.hpp"
#include "r2s_bridge/service_types.hpp"
#include "r2s_bridge/server_waker.hpp"

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

// One raw rcl service server on the node, the way a route holds one where
// the middleware has no generic service: the waker watches the handle,
// never the route.
class RawService
{
public:
  RawService(rclcpp::Node::SharedPtr node, const std::string & name)
  : node_(std::move(node)), types_("std_srvs/srv/Trigger")
  {
    rcl_node_t * node_handle = node_->get_node_base_interface()->get_shared_rcl_node_handle().get();
    const rcl_service_options_t options = rcl_service_get_default_options();
    if (
      rcl_service_init(
        &server_, node_handle, types_.cpp_service_handle(), name.c_str(), &options) !=
      RCL_RET_OK)
    {
      rcl_reset_error();
      throw std::runtime_error("cannot init the raw service server");
    }
  }
  ~RawService()
  {
    rcl_node_t * node_handle = node_->get_node_base_interface()->get_shared_rcl_node_handle().get();
    if (rcl_service_fini(&server_, node_handle) != RCL_RET_OK) {
      rcl_reset_error();
    }
  }
  RawService(const RawService &) = delete;
  RawService & operator=(const RawService &) = delete;

  const rcl_service_t * handle() const {return &server_;}

private:
  rclcpp::Node::SharedPtr node_;
  ServiceTypes types_;
  rcl_service_t server_ = rcl_get_zero_initialized_service();
};

class ServerWakerTest : public ::testing::Test
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

// A waker needs a context for its wait set and a callback to run: both are
// checked up front, before any thread starts.
TEST_F(ServerWakerTest, RejectsANullContext)
{
  EXPECT_THROW(ServerWaker(nullptr, []() {}), std::invalid_argument);
}

TEST_F(ServerWakerTest, RejectsAnEmptyCallback)
{
  ServerWaker::ReadyCallback ready;
  EXPECT_THROW(
    ServerWaker(node_->get_node_base_interface()->get_context(), ready), std::invalid_argument);
}

// A goal request wakes the waiter: the take it announces runs on the
// executor's transmit, not on the watcher thread.
TEST_F(ServerWakerTest, WakesWhenAGoalArrives)
{
  RawServer server(node_, "spin");
  std::atomic<bool> fired{false};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
      fired.store(true);
    });
  waker.watch({server.handle()}, {});
  auto client = rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(node_, "spin");
  ASSERT_TRUE(client->wait_for_action_server(std::chrono::seconds(10)));
  tf2_msgs::action::LookupTransform_Goal goal;
  goal.target_frame = "base";
  client->async_send_goal(goal);
  ASSERT_TRUE(spin_until_triggered(node_, fired)) << "a goal must wake the waiter";
  waker.unwatch();
}

// No servers means no wakeups: the waiter parks instead of polling.
TEST_F(ServerWakerTest, StaysQuietWithNoServers)
{
  std::atomic<bool> fired{false};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
      fired.store(true);
    });
  waker.watch({}, {});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(fired.load()) << "an empty watch must not wake";
  waker.unwatch();
}

// A watched server with no traffic stays quiet: the waiter blocks in the
// wait instead of spinning on an idle set.
TEST_F(ServerWakerTest, StaysQuietOnAnIdleServer)
{
  RawServer server(node_, "spin");
  std::atomic<bool> fired{false};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
      fired.store(true);
    });
  waker.watch({server.handle()}, {});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(fired.load()) << "an idle server must not wake";
  waker.unwatch();
}

// Unwatching drops the set: a goal that arrives after the drop wakes
// nothing, even though its entity is still ready.
TEST_F(ServerWakerTest, UnwatchStopsTheWakeups)
{
  RawServer server(node_, "spin");
  std::atomic<bool> fired{false};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
      fired.store(true);
    });
  waker.watch({server.handle()}, {});
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
TEST_F(ServerWakerTest, WatchSwapsTheWatchedSet)
{
  RawServer first(node_, "spin");
  RawServer second(node_, "spin2");
  std::atomic<int> fires{0};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fires]() {
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
  waker.watch({first.handle()}, {});
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
  waker.watch({second.handle()}, {});
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

// Watching the same set again changes nothing: the helper keeps waiting
// instead of disarming and rebuilding, and the next arrival still wakes.
TEST_F(ServerWakerTest, WatchingTheSameSetSkipsTheRebuild)
{
  RawServer server(node_, "spin");
  std::atomic<int> fires{0};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fires]() {
      ++fires;
    });
  waker.watch({server.handle()}, {});
  // An identical watch is a no-op: the server stays watched.
  waker.watch({server.handle()}, {});
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
  EXPECT_EQ(fires.load(), 1) << "a goal must wake after an identical re-watch";
  waker.unwatch();
}

// Watching new services beside watched actions re-arms: the action half
// stays watched while the service half joins it.
TEST_F(ServerWakerTest, WatchingNewServicesKeepsTheActions)
{
  RawServer server(node_, "spin");
  RawService service(node_, "reset");
  std::atomic<int> fires{0};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fires]() {
      ++fires;
    });
  waker.watch({server.handle()}, {});
  // Same actions, new services: the set changed, so the waiter rebuilds.
  waker.watch({server.handle()}, {service.handle()});
  auto goal_client =
    rclcpp_action::create_client<tf2_msgs::action::LookupTransform>(node_, "spin");
  ASSERT_TRUE(goal_client->wait_for_action_server(std::chrono::seconds(10)));
  auto service_client = node_->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(service_client->wait_for_service(std::chrono::seconds(10)));
  tf2_msgs::action::LookupTransform_Goal goal;
  goal.target_frame = "base";
  goal_client->async_send_goal(goal);
  const auto first = std::chrono::steady_clock::now();
  while (fires.load() < 1 &&
    std::chrono::steady_clock::now() - first < std::chrono::seconds(10))
  {
    rclcpp::spin_some(node_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(fires.load(), 1) << "a goal must wake after the services join";
  ASSERT_TRUE(server.take_goal());
  waker.notify_served(true);
  auto future =
    service_client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  const auto second = std::chrono::steady_clock::now();
  while (fires.load() < 2 &&
    std::chrono::steady_clock::now() - second < std::chrono::seconds(10))
  {
    rclcpp::spin_some(node_);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(fires.load(), 2) << "a request must wake after the services join";
  waker.unwatch();
}

// A served wakeup re-arms: the next goal on the same server wakes again.
TEST_F(ServerWakerTest, ServedRearmsTheWaiter)
{
  RawServer server(node_, "spin");
  std::atomic<int> fires{0};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fires]() {
      ++fires;
    });
  waker.watch({server.handle()}, {});
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

// A service request wakes the waiter where the middleware has no generic
// service: the take it announces runs on the executor's transmit.
TEST_F(ServerWakerTest, WakesWhenAServiceRequestArrives)
{
  RawService server(node_, "reset");
  std::atomic<bool> fired{false};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
      fired.store(true);
    });
  waker.watch({}, {server.handle()});
  auto client = node_->create_client<std_srvs::srv::Trigger>("reset");
  ASSERT_TRUE(client->wait_for_service(std::chrono::seconds(10)));
  auto future = client->async_send_request(std::make_shared<std_srvs::srv::Trigger::Request>());
  ASSERT_TRUE(spin_until_triggered(node_, fired)) << "a request must wake the waiter";
  waker.unwatch();
}

// A watched service server with no traffic stays quiet: the waiter blocks
// in the wait instead of spinning on an idle set.
TEST_F(ServerWakerTest, StaysQuietOnAnIdleServiceServer)
{
  RawService server(node_, "reset");
  std::atomic<bool> fired{false};
  ServerWaker waker(node_->get_node_base_interface()->get_context(), [&fired]() {
      fired.store(true);
    });
  waker.watch({}, {server.handle()});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(fired.load()) << "an idle service server must not wake";
  waker.unwatch();
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
