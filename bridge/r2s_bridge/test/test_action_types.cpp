// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors
// The action introspection handles: names, sizes, live structs, envelope
// field access against the real generated types, and the unknown-type
// refusal behind every routed action.
// covers: ACT-1
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <action_msgs/srv/cancel_goal.hpp>
#include <rclcpp/rclcpp.hpp>
#include <tf2_msgs/action/lookup_transform.hpp>

#include "r2s_bridge/action_types.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

TEST(ActionTypes, LoadsLookupTransformHandles)
{
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  EXPECT_EQ(types.goal_type_name(), "tf2_msgs/action/LookupTransform_Goal");
  EXPECT_EQ(types.result_type_name(), "tf2_msgs/action/LookupTransform_Result");
  EXPECT_EQ(types.feedback_type_name(), "tf2_msgs/action/LookupTransform_Feedback");
  EXPECT_NE(types.action_handle(), nullptr);
  EXPECT_NE(types.cpp_action_handle(), nullptr);
  EXPECT_EQ(types.make_goal().storage.size(), sizeof(tf2_msgs::action::LookupTransform_Goal));
  EXPECT_EQ(
    types.make_result().storage.size(), sizeof(tf2_msgs::action::LookupTransform_Result));
  EXPECT_EQ(
    types.make_feedback().storage.size(), sizeof(tf2_msgs::action::LookupTransform_Feedback));
}

TEST(ActionTypes, RejectsUnknownType)
{
  EXPECT_THROW(ActionTypes("nope/action/Nope"), std::runtime_error);
}

TEST(ActionTypes, MovesAndFreesStructs)
{
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  ActionTypes::Struct first = types.make_goal_request();
  ActionTypes::Struct second = types.make_goal_request();
  const std::size_t size = first.storage.size();
  EXPECT_GT(size, 0u);
  // Assigning over a live struct finalizes it first; the source goes empty.
  second = std::move(first);
  EXPECT_EQ(second.storage.size(), size);
  EXPECT_EQ(first.storage.size(), 0u);
  // Assigning over an empty struct skips the finalizer.
  ActionTypes::Struct third;
  third = std::move(second);
  EXPECT_EQ(third.storage.size(), size);
  EXPECT_EQ(second.storage.size(), 0u);
}

// The layout the rcl server reads through must match the generated C++
// types: fill through the introspection handles, read back through the
// real structs.
TEST(ActionTypes, GoalResponseMatchesGeneratedLayout)
{
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  ActionTypes::Struct response = types.make_goal_response();
  ASSERT_TRUE(types.fill_goal_response(response, 7, 9u, true));
  const auto *real = reinterpret_cast<
    const tf2_msgs::action::LookupTransform_SendGoal_Response *>(response.storage.data());
  EXPECT_EQ(real->stamp.sec, 7);
  EXPECT_EQ(real->stamp.nanosec, 9u);
  EXPECT_TRUE(real->accepted);
}

TEST(ActionTypes, GoalRequestReadsGeneratedLayout)
{
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  ActionTypes::Struct request = types.make_goal_request();
  ASSERT_EQ(
    request.storage.size(), sizeof(tf2_msgs::action::LookupTransform_SendGoal_Request));
  // The goal id is plain bytes, so it copies without touching the heap;
  // strings stay default-initialized, which the finalizer frees safely.
  auto *base = request.storage.data();
  for (std::size_t i = 0; i < 16u; ++i) {
    base[offsetof(tf2_msgs::action::LookupTransform_SendGoal_Request, goal_id) + i] =
      static_cast<std::uint8_t>(i + 1u);
  }
  std::array<std::uint8_t, 16> goal_id{};
  ASSERT_TRUE(types.goal_id_of(request, goal_id));
  for (std::size_t i = 0; i < goal_id.size(); ++i) {
    EXPECT_EQ(goal_id[i], static_cast<std::uint8_t>(i + 1u));
  }
  const void *goal = types.goal_of(request);
  ASSERT_NE(goal, nullptr);
  EXPECT_EQ(
    static_cast<const std::uint8_t *>(goal) - base,
    static_cast<std::ptrdiff_t>(
      offsetof(tf2_msgs::action::LookupTransform_SendGoal_Request, goal)));
}

TEST(ActionTypes, ResultResponseMatchesGeneratedLayout)
{
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  ActionTypes::Struct response = types.make_result_response();
  ASSERT_TRUE(types.fill_result_status(response, 4));
  const auto *real = reinterpret_cast<
    const tf2_msgs::action::LookupTransform_GetResult_Response *>(response.storage.data());
  EXPECT_EQ(real->status, 4);
  EXPECT_NE(types.result_of(response), nullptr);
}

TEST(ActionTypes, FeedbackMessageMatchesGeneratedLayout)
{
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  ActionTypes::Struct message = types.make_feedback_message();
  const std::array<std::uint8_t, 16> goal_id{1u, 2u, 3u};
  ASSERT_TRUE(types.fill_feedback_id(message, goal_id));
  const auto *real = reinterpret_cast<
    const tf2_msgs::action::LookupTransform_FeedbackMessage *>(message.storage.data());
  EXPECT_EQ(real->goal_id.uuid[0], 1u);
  EXPECT_EQ(real->goal_id.uuid[1], 2u);
  EXPECT_EQ(real->goal_id.uuid[2], 3u);
  EXPECT_NE(types.feedback_of(message), nullptr);
}

TEST(ActionTypes, RefusesStructsWithoutTheEnvelopeFields)
{
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  ActionTypes::Struct goal = types.make_goal();
  std::array<std::uint8_t, 16> goal_id{};
  std::int32_t sec = 0;
  std::uint32_t nanosec = 0u;
  EXPECT_FALSE(types.goal_id_of(goal, goal_id));
  EXPECT_EQ(types.goal_of(goal), nullptr);
  EXPECT_FALSE(types.fill_goal_response(goal, 0, 0u, true));
  EXPECT_FALSE(types.result_id_of(goal, goal_id));
  EXPECT_FALSE(types.fill_result_status(goal, 0));
  EXPECT_EQ(types.result_of(goal), nullptr);
  EXPECT_FALSE(types.fill_feedback_id(goal, goal_id));
  EXPECT_EQ(types.feedback_of(goal), nullptr);
  EXPECT_FALSE(types.cancel_of(goal, goal_id, sec, nanosec));
  ActionTypes::Struct response = types.make_goal_response();
  EXPECT_FALSE(types.fill_cancel_response(response, 0, goal_id, 0, 0u, true));
  ActionTypes::Struct empty;
  EXPECT_FALSE(types.goal_id_of(empty, goal_id));
  EXPECT_EQ(types.goal_of(empty), nullptr);
  EXPECT_FALSE(types.fill_goal_response(empty, 0, 0u, true));
  EXPECT_EQ(types.result_of(empty), nullptr);
  EXPECT_EQ(types.feedback_of(empty), nullptr);
}

TEST(ActionTypes, CancelExchangeMatchesGeneratedLayout)
{
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  action_msgs::srv::CancelGoal_Request real_request;
  real_request.goal_info.goal_id.uuid[0] = 0xABu;
  real_request.goal_info.stamp.sec = 3;
  real_request.goal_info.stamp.nanosec = 4u;
  ActionTypes::Struct request = types.make_cancel_request();
  ASSERT_EQ(request.storage.size(), sizeof(real_request));
  std::memcpy(request.storage.data(), &real_request, sizeof(real_request));
  std::array<std::uint8_t, 16> goal_id{};
  std::int32_t sec = 0;
  std::uint32_t nanosec = 0u;
  ASSERT_TRUE(types.cancel_of(request, goal_id, sec, nanosec));
  EXPECT_EQ(goal_id[0], 0xABu);
  EXPECT_EQ(sec, 3);
  EXPECT_EQ(nanosec, 4u);

  ActionTypes::Struct response = types.make_cancel_response();
  const std::array<std::uint8_t, 16> cancel_id{0xCDu};
  ASSERT_TRUE(types.fill_cancel_response(response, 0, cancel_id, 5, 6u, true));
  const auto *real = reinterpret_cast<const action_msgs::srv::CancelGoal_Response *>(
    response.storage.data());
  EXPECT_EQ(real->return_code, 0);
  ASSERT_EQ(real->goals_canceling.size(), 1u);
  EXPECT_EQ(real->goals_canceling[0].goal_id.uuid[0], 0xCDu);
  EXPECT_EQ(real->goals_canceling[0].stamp.sec, 5);
  EXPECT_EQ(real->goals_canceling[0].stamp.nanosec, 6u);

  ActionTypes::Struct empty = types.make_cancel_response();
  ASSERT_TRUE(types.fill_cancel_response(empty, 1, cancel_id, 0, 0u, false));
  const auto *empty_real = reinterpret_cast<const action_msgs::srv::CancelGoal_Response *>(
    empty.storage.data());
  EXPECT_EQ(empty_real->return_code, 1);
  EXPECT_TRUE(empty_real->goals_canceling.empty());
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge

int main(int argc, char ** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  const int code = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return code;
}
