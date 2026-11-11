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
#include <rosidl_typesupport_introspection_cpp/field_types.hpp>
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
  EXPECT_THROW(ActionTypes("std_msgs/msg/String"), std::runtime_error)
    << "a message is not an action: it has no action symbol";
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
  // Moving constructs the target the same way: the source goes empty.
  const auto *members = third.members;
  ActionTypes::Struct fourth = std::move(third);
  EXPECT_EQ(fourth.storage.size(), size);
  EXPECT_EQ(fourth.members, members);
  EXPECT_EQ(third.storage.size(), 0u);
  EXPECT_EQ(third.members, nullptr);
  // Assigning from a struct whose storage was released skips the finalizer.
  ActionTypes::Struct cleared = types.make_goal();
  cleared.storage.clear();
  const auto *goal_members = cleared.members;
  ActionTypes::Struct fifth;
  fifth = std::move(cleared);
  EXPECT_EQ(fifth.members, goal_members);
  EXPECT_TRUE(fifth.storage.empty());
  // Self-assignment is a guarded no-op.
  ActionTypes::Struct self = types.make_goal();
  const auto *self_members = self.members;
  const auto self_size = self.storage.size();
  ActionTypes::Struct & alias = self;
  alias = std::move(self);
  EXPECT_EQ(alias.members, self_members);
  EXPECT_EQ(alias.storage.size(), self_size);
}

// Assigning over a struct whose storage was released but whose members
// survive skips the finalizer: there is nothing initialized to free.
TEST(ActionTypes, MoveAssignOverClearedStorageSkipsFini)
{
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  ActionTypes::Struct cleared = types.make_goal();
  cleared.storage.clear();
  ASSERT_NE(cleared.members, nullptr);
  ActionTypes::Struct source = types.make_goal();
  const std::size_t size = source.storage.size();
  cleared = std::move(source);
  EXPECT_EQ(cleared.storage.size(), size);
  EXPECT_EQ(source.storage.size(), 0u);
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
  EXPECT_FALSE(types.fill_cancel_response(response, 0, goal_id, 0, 0u, true))
    << "a goal response carries no goals to cancel";
  ActionTypes::Struct result = types.make_result();
  EXPECT_FALSE(types.fill_goal_response(result, 0, 0u, true))
    << "a result carries no acceptance to stamp";
  ActionTypes::Struct empty;
  EXPECT_FALSE(types.fill_cancel_response(empty, 0, goal_id, 0, 0u, true));
  EXPECT_FALSE(types.goal_id_of(empty, goal_id));
  EXPECT_EQ(types.goal_of(empty), nullptr);
  EXPECT_FALSE(types.fill_goal_response(empty, 0, 0u, true));
  EXPECT_FALSE(types.result_id_of(empty, goal_id));
  EXPECT_FALSE(types.fill_result_status(empty, 0));
  EXPECT_EQ(types.result_of(empty), nullptr);
  EXPECT_FALSE(types.fill_feedback_id(empty, goal_id));
  EXPECT_EQ(types.feedback_of(empty), nullptr);
  EXPECT_FALSE(types.cancel_of(empty, goal_id, sec, nanosec));
  // A struct whose storage was released keeps its members but answers
  // nothing: every accessor guards the empty storage as well.
  ActionTypes::Struct cleared = types.make_goal_request();
  cleared.storage.clear();
  EXPECT_FALSE(types.goal_id_of(cleared, goal_id));
  EXPECT_EQ(types.goal_of(cleared), nullptr);
  EXPECT_FALSE(types.fill_goal_response(cleared, 0, 0u, true));
  EXPECT_FALSE(types.result_id_of(cleared, goal_id));
  EXPECT_FALSE(types.fill_result_status(cleared, 0));
  EXPECT_EQ(types.result_of(cleared), nullptr);
  EXPECT_FALSE(types.fill_feedback_id(cleared, goal_id));
  EXPECT_EQ(types.feedback_of(cleared), nullptr);
  EXPECT_FALSE(types.cancel_of(cleared, goal_id, sec, nanosec));
  EXPECT_FALSE(types.fill_cancel_response(cleared, 0, goal_id, 0, 0u, true));
}

TEST(ActionTypes, RejectsHandBuiltLayouts)
{
  using rosidl_typesupport_introspection_cpp::MessageMember;
  using rosidl_typesupport_introspection_cpp::MessageMembers;
  using rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE;
  using rosidl_typesupport_introspection_cpp::ROS_TYPE_STRING;
  using rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8;
  const ActionTypes types("tf2_msgs/action/LookupTransform");
  std::array<std::uint8_t, 16> goal_id{};
  // A layout with no fields finds nothing: every accessor refuses it.
  // The members are built field by field so the test does not depend on
  // the C struct initializer order.
  MessageMembers no_fields{};
  no_fields.member_count_ = 0u;
  no_fields.members_ = nullptr;
  ActionTypes::Struct missing;
  missing.members = &no_fields;
  missing.storage.resize(16u, 0u);
  EXPECT_FALSE(types.goal_id_of(missing, goal_id));
  EXPECT_EQ(types.goal_of(missing), nullptr);
  EXPECT_FALSE(types.result_id_of(missing, goal_id));
  EXPECT_EQ(types.result_of(missing), nullptr);
  EXPECT_EQ(types.feedback_of(missing), nullptr);
  // A field with the wrong type is not a sub-message.
  MessageMember wrong_type{};
  wrong_type.name_ = "goal_id";
  wrong_type.type_id_ = ROS_TYPE_STRING;
  MessageMembers wrong_parent{};
  wrong_parent.member_count_ = 1u;
  wrong_parent.members_ = &wrong_type;
  ActionTypes::Struct wrong;
  wrong.members = &wrong_parent;
  wrong.storage.resize(16u, 0u);
  EXPECT_FALSE(types.goal_id_of(wrong, goal_id));
  // A message field without sub-members is refused.
  MessageMember no_sub{};
  no_sub.name_ = "goal_id";
  no_sub.type_id_ = ROS_TYPE_MESSAGE;
  no_sub.members_ = nullptr;
  MessageMembers no_sub_parent{};
  no_sub_parent.member_count_ = 1u;
  no_sub_parent.members_ = &no_sub;
  ActionTypes::Struct no_sub_struct;
  no_sub_struct.members = &no_sub_parent;
  no_sub_struct.storage.resize(16u, 0u);
  EXPECT_FALSE(types.goal_id_of(no_sub_struct, goal_id));
  // A message field whose type support carries no data is refused.
  rosidl_message_type_support_t empty_ts{};
  empty_ts.data = nullptr;
  MessageMember no_data{};
  no_data.name_ = "goal_id";
  no_data.type_id_ = ROS_TYPE_MESSAGE;
  no_data.members_ = &empty_ts;
  MessageMembers no_data_parent{};
  no_data_parent.member_count_ = 1u;
  no_data_parent.members_ = &no_data;
  ActionTypes::Struct no_data_struct;
  no_data_struct.members = &no_data_parent;
  no_data_struct.storage.resize(16u, 0u);
  EXPECT_FALSE(types.goal_id_of(no_data_struct, goal_id));
  // A uuid entry with the wrong type is not a goal id: the bytes check
  // sees a non-null member and evaluates each shape term.
  MessageMember bad_type_bytes{};
  bad_type_bytes.name_ = "uuid";
  bad_type_bytes.type_id_ = ROS_TYPE_STRING;
  bad_type_bytes.is_array_ = true;
  bad_type_bytes.array_size_ = 16u;
  MessageMembers bad_type_uuid{};
  bad_type_uuid.member_count_ = 1u;
  bad_type_uuid.members_ = &bad_type_bytes;
  rosidl_message_type_support_t bad_type_ts{};
  bad_type_ts.data = &bad_type_uuid;
  MessageMember bad_type_goal{};
  bad_type_goal.name_ = "goal_id";
  bad_type_goal.type_id_ = ROS_TYPE_MESSAGE;
  bad_type_goal.members_ = &bad_type_ts;
  MessageMembers bad_type_parent{};
  bad_type_parent.member_count_ = 1u;
  bad_type_parent.members_ = &bad_type_goal;
  ActionTypes::Struct bad_type_struct;
  bad_type_struct.members = &bad_type_parent;
  bad_type_struct.storage.resize(64u, 0u);
  EXPECT_FALSE(types.goal_id_of(bad_type_struct, goal_id));
  // A uuid entry that is not an array is refused the same way.
  MessageMember no_array_bytes{};
  no_array_bytes.name_ = "uuid";
  no_array_bytes.type_id_ = ROS_TYPE_UINT8;
  no_array_bytes.is_array_ = false;
  no_array_bytes.array_size_ = 16u;
  MessageMembers no_array_uuid{};
  no_array_uuid.member_count_ = 1u;
  no_array_uuid.members_ = &no_array_bytes;
  rosidl_message_type_support_t no_array_ts{};
  no_array_ts.data = &no_array_uuid;
  MessageMember no_array_goal{};
  no_array_goal.name_ = "goal_id";
  no_array_goal.type_id_ = ROS_TYPE_MESSAGE;
  no_array_goal.members_ = &no_array_ts;
  MessageMembers no_array_parent{};
  no_array_parent.member_count_ = 1u;
  no_array_parent.members_ = &no_array_goal;
  ActionTypes::Struct no_array_struct;
  no_array_struct.members = &no_array_parent;
  no_array_struct.storage.resize(64u, 0u);
  EXPECT_FALSE(types.goal_id_of(no_array_struct, goal_id));
  // A uuid array of the wrong size is refused as well.
  MessageMember bad_size_bytes{};
  bad_size_bytes.name_ = "uuid";
  bad_size_bytes.type_id_ = ROS_TYPE_UINT8;
  bad_size_bytes.is_array_ = true;
  bad_size_bytes.array_size_ = 8u;
  MessageMembers bad_size_uuid{};
  bad_size_uuid.member_count_ = 1u;
  bad_size_uuid.members_ = &bad_size_bytes;
  rosidl_message_type_support_t bad_size_ts{};
  bad_size_ts.data = &bad_size_uuid;
  MessageMember bad_size_goal{};
  bad_size_goal.name_ = "goal_id";
  bad_size_goal.type_id_ = ROS_TYPE_MESSAGE;
  bad_size_goal.members_ = &bad_size_ts;
  MessageMembers bad_size_parent{};
  bad_size_parent.member_count_ = 1u;
  bad_size_parent.members_ = &bad_size_goal;
  ActionTypes::Struct bad_size_struct;
  bad_size_struct.members = &bad_size_parent;
  bad_size_struct.storage.resize(64u, 0u);
  EXPECT_FALSE(types.goal_id_of(bad_size_struct, goal_id));
  // A status field with the wrong type is not an int8.
  MessageMember bad_status{};
  bad_status.name_ = "status";
  bad_status.type_id_ = ROS_TYPE_STRING;
  MessageMembers bad_status_parent{};
  bad_status_parent.member_count_ = 1u;
  bad_status_parent.members_ = &bad_status;
  ActionTypes::Struct bad_status_struct;
  bad_status_struct.members = &bad_status_parent;
  bad_status_struct.storage.resize(16u, 0u);
  EXPECT_FALSE(types.fill_result_status(bad_status_struct, 0));
  bad_status_struct.members = nullptr;
  // The write path refuses the same bad uuid layouts.
  EXPECT_FALSE(types.fill_feedback_id(bad_type_struct, goal_id));
  EXPECT_FALSE(types.fill_feedback_id(no_array_struct, goal_id));
  EXPECT_FALSE(types.fill_feedback_id(bad_size_struct, goal_id));
  bad_type_struct.members = nullptr;
  no_array_struct.members = nullptr;
  bad_size_struct.members = nullptr;
  // The fakes own no ROS storage: detach them before they destruct so no
  // finalizer runs on the hand-built members.
  missing.members = nullptr;
  wrong.members = nullptr;
  no_sub_struct.members = nullptr;
  no_data_struct.members = nullptr;
  bad_type_struct.members = nullptr;
  no_array_struct.members = nullptr;
  bad_size_struct.members = nullptr;
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
