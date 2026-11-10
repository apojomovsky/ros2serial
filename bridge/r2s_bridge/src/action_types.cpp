// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors
// The introspection handles behind one action type, loaded by name.
#include "r2s_bridge/action_types.hpp"

#include <cstring>
#include <stdexcept>

#include "rclcpp/typesupport_helpers.hpp"
#include "rosidl_runtime_cpp/message_initialization.hpp"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"

namespace r2s_bridge
{

namespace
{

constexpr const char *kIntrospectionId = "rosidl_typesupport_introspection_cpp";

using rosidl_typesupport_introspection_cpp::MessageMember;
using rosidl_typesupport_introspection_cpp::MessageMembers;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_BOOLEAN;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_INT32;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_INT8;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT32;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8;

const MessageMember * find_member(const MessageMembers *members, const char *name)
{
  if (members == nullptr) {
    return nullptr;
  }
  for (uint32_t i = 0; i < members->member_count_; ++i) {
    if (std::strcmp(members->members_[i].name_, name) == 0) {
      return &members->members_[i];
    }
  }
  return nullptr;
}

const MessageMembers * nested_members(const MessageMember *member)
{
  // GCOVR_EXCL_BR_START: installed members always describe their
  // sub-messages; only a hand-built layout could miss them.
  if (member == nullptr || member->type_id_ != ROS_TYPE_MESSAGE ||
    member->members_ == nullptr || member->members_->data == nullptr)
  {
    return nullptr;
  }
  // GCOVR_EXCL_BR_STOP
  return static_cast<const MessageMembers *>(member->members_->data);
}

bool read_uuid(
  const MessageMembers *parent, const void *storage, const char *field,
  std::array<std::uint8_t, 16> &goal_id)
{
  const MessageMember *member = find_member(parent, field);
  const MessageMembers *uuid_members = nested_members(member);
  const MessageMember *bytes = find_member(uuid_members, "uuid");
  // GCOVR_EXCL_BR_START: installed ids are always 16 plain bytes;
  // only a hand-built layout could miss them.
  if (bytes == nullptr || bytes->type_id_ != ROS_TYPE_UINT8 || !bytes->is_array_ ||
    bytes->array_size_ != 16u)
  {
    return false;
  }
  // GCOVR_EXCL_BR_STOP
  const auto *base = static_cast<const std::uint8_t *>(storage) + member->offset_;
  std::memcpy(goal_id.data(), base + bytes->offset_, goal_id.size());
  return true;
}

bool read_time(
  const MessageMembers *parent, const void *storage, const char *field, std::int32_t &sec,
  std::uint32_t &nanosec)
{
  const MessageMember *member = find_member(parent, field);
  const MessageMembers *time_members = nested_members(member);
  const MessageMember *sec_member = find_member(time_members, "sec");
  const MessageMember *nsec_member = find_member(time_members, "nanosec");
  // GCOVR_EXCL_START: installed time fields always carry sec and nanosec;
  // only a hand-built layout could miss them.
  if (sec_member == nullptr || sec_member->type_id_ != ROS_TYPE_INT32 ||
    nsec_member == nullptr || nsec_member->type_id_ != ROS_TYPE_UINT32)
  {
    return false;
  }
  // GCOVR_EXCL_STOP
  const auto *base = static_cast<const std::uint8_t *>(storage) + member->offset_;
  std::memcpy(&sec, base + sec_member->offset_, sizeof(sec));
  std::memcpy(&nanosec, base + nsec_member->offset_, sizeof(nanosec));
  return true;
}

bool write_uuid(
  const MessageMembers *parent, void *storage, const char *field,
  const std::array<std::uint8_t, 16> &goal_id)
{
  const MessageMember *member = find_member(parent, field);
  const MessageMembers *uuid_members = nested_members(member);
  const MessageMember *bytes = find_member(uuid_members, "uuid");
  // GCOVR_EXCL_BR_START: installed ids are always 16 plain bytes;
  // only a hand-built layout could miss them.
  if (bytes == nullptr || bytes->type_id_ != ROS_TYPE_UINT8 || !bytes->is_array_ ||
    bytes->array_size_ != 16u)
  {
    return false;
  }
  // GCOVR_EXCL_BR_STOP
  auto *base = static_cast<std::uint8_t *>(storage) + member->offset_;
  std::memcpy(base + bytes->offset_, goal_id.data(), goal_id.size());
  return true;
}

bool write_time(
  const MessageMembers *parent, void *storage, const char *field, std::int32_t sec,
  std::uint32_t nanosec)
{
  const MessageMember *member = find_member(parent, field);
  const MessageMembers *time_members = nested_members(member);
  const MessageMember *sec_member = find_member(time_members, "sec");
  const MessageMember *nsec_member = find_member(time_members, "nanosec");
  if (sec_member == nullptr || sec_member->type_id_ != ROS_TYPE_INT32 ||
    nsec_member == nullptr || nsec_member->type_id_ != ROS_TYPE_UINT32)
  {
    return false;
  }
  auto *base = static_cast<std::uint8_t *>(storage) + member->offset_;
  std::memcpy(base + sec_member->offset_, &sec, sizeof(sec));
  std::memcpy(base + nsec_member->offset_, &nanosec, sizeof(nanosec));
  return true;
}

bool write_bool(const MessageMembers *parent, void *storage, const char *field, bool value)
{
  const MessageMember *member = find_member(parent, field);
  // GCOVR_EXCL_START: installed bool fields always match; only a
  // hand-built layout could miss them.
  if (member == nullptr || member->type_id_ != ROS_TYPE_BOOLEAN) {
    return false;
  }
  // GCOVR_EXCL_STOP
  auto *base = static_cast<std::uint8_t *>(storage);
  const std::uint8_t byte = value ? 1u : 0u;
  std::memcpy(base + member->offset_, &byte, sizeof(byte));
  return true;
}

bool write_int8(const MessageMembers *parent, void *storage, const char *field, std::int8_t value)
{
  const MessageMember *member = find_member(parent, field);
  // GCOVR_EXCL_BR_START: installed int8 fields always match; only a
  // hand-built layout could miss them.
  if (member == nullptr || member->type_id_ != ROS_TYPE_INT8) {
    return false;
  }
  // GCOVR_EXCL_BR_STOP
  auto *base = static_cast<std::uint8_t *>(storage);
  std::memcpy(base + member->offset_, &value, sizeof(value));
  return true;
}

const void * nested_storage(
  const MessageMembers *parent, const void *storage, const char *field)
{
  const MessageMember *member = find_member(parent, field);
  if (nested_members(member) == nullptr) {
    return nullptr;
  }
  const auto *base = static_cast<const std::uint8_t *>(storage);
  return base + member->offset_;
}

void * nested_storage(const MessageMembers *parent, void *storage, const char *field)
{
  const MessageMember *member = find_member(parent, field);
  if (nested_members(member) == nullptr) {
    return nullptr;
  }
  auto *base = static_cast<std::uint8_t *>(storage);
  return base + member->offset_;
}

std::string mangle(const std::string &type_name)
{
  std::string out;
  for (const char c : type_name) {
    out += (c == '/') ? "__" : std::string(1, c);
  }
  return out;
}  // GCOVR_EXCL_LINE: the close executes on every return; gcovr leaves it cold

}  // namespace

ActionTypes::Struct::Struct(Struct && other) noexcept
: members(other.members), storage(std::move(other.storage))
{
  other.members = nullptr;
}

ActionTypes::Struct & ActionTypes::Struct::operator=(Struct && other) noexcept
{
  if (this != &other) {
    if (members != nullptr && !storage.empty()) {
      members->fini_function(storage.data());
    }
    members = other.members;
    storage = std::move(other.storage);
    other.members = nullptr;
  }
  return *this;
}

ActionTypes::Struct::~Struct()
{
  if (members != nullptr && !storage.empty()) {
    members->fini_function(storage.data());
  }
}

ActionTypes::ActionTypes(const std::string & type_name)
: type_name_(type_name)
{
  try {
    constexpr const char *kCppId = "rosidl_typesupport_cpp";
    cpp_library_ = rclcpp::get_typesupport_library(type_name, kCppId);
    const std::string symbol =
      "rosidl_typesupport_cpp__get_action_type_support_handle__" + mangle(type_name);
    // get_symbol throws when the action symbol is missing, so a null
    // check here would never fire.
    void *untyped = cpp_library_->get_symbol(symbol);
    const auto get_handle =
      reinterpret_cast<const rosidl_action_type_support_t * (*)()>(untyped);
    cpp_action_ts_ = get_handle();
    library_ = rclcpp::get_typesupport_library(type_name, kIntrospectionId);
    const auto message_members = [this](const std::string & message) {
        const rosidl_message_type_support_t *ts =
          rclcpp::get_message_typesupport_handle(message, kIntrospectionId, *library_);
        return static_cast<const MessageMembers *>(ts->data);
      };
    goal_members_ = message_members(type_name + "_Goal");
    result_members_ = message_members(type_name + "_Result");
    feedback_members_ = message_members(type_name + "_Feedback");
    goal_request_members_ = message_members(type_name + "_SendGoal_Request");
    goal_response_members_ = message_members(type_name + "_SendGoal_Response");
    result_request_members_ = message_members(type_name + "_GetResult_Request");
    result_response_members_ = message_members(type_name + "_GetResult_Response");
    feedback_message_members_ = message_members(type_name + "_FeedbackMessage");
    cancel_library_ =
      rclcpp::get_typesupport_library("action_msgs/srv/CancelGoal", kIntrospectionId);
    const auto cancel_members = [this](const std::string & message) {
        const rosidl_message_type_support_t *ts =
          rclcpp::get_message_typesupport_handle(message, kIntrospectionId, *cancel_library_);
        return static_cast<const MessageMembers *>(ts->data);
      };
    cancel_request_members_ = cancel_members("action_msgs/srv/CancelGoal_Request");
    cancel_response_members_ = cancel_members("action_msgs/srv/CancelGoal_Response");
    action_ts_ = cpp_action_ts_;
  } catch (const std::exception & failure) {
    throw std::runtime_error("action '" + type_name + "': no introspection type support (" +
      failure.what() + ")");
  }
  // GCOVR_EXCL_START: the lookups above throw when a symbol is missing,
  // so a null without a throw would break their documented contract.
  // A unit test cannot stage the state these guards describe.
  if (goal_members_ == nullptr || result_members_ == nullptr || feedback_members_ == nullptr ||
    goal_request_members_ == nullptr || goal_response_members_ == nullptr ||
    result_request_members_ == nullptr || result_response_members_ == nullptr ||
    feedback_message_members_ == nullptr || cancel_request_members_ == nullptr ||
    cancel_response_members_ == nullptr || action_ts_ == nullptr)
  {
    throw std::runtime_error("action '" + type_name + "': no introspection type support");
  }
  if (goal_members_->init_function == nullptr || goal_members_->fini_function == nullptr) {
    throw std::runtime_error("action '" + type_name + "': no init or fini function");
  }
  // GCOVR_EXCL_STOP
}

ActionTypes::~ActionTypes()
{
  // The members point into the library, which outlives this object.
}

std::string ActionTypes::goal_type_name() const
{
  return type_name_ + "_Goal";
}

std::string ActionTypes::result_type_name() const
{
  return type_name_ + "_Result";
}

std::string ActionTypes::feedback_type_name() const
{
  return type_name_ + "_Feedback";
}

std::array<std::uint8_t, 8> ActionTypes::type_hash() const
{
  const rosidl_type_hash_t *installed = action_ts_->get_type_hash_func(action_ts_);
  std::array<std::uint8_t, 8> out{};
  std::copy(installed->value, installed->value + out.size(), out.begin());
  return out;
}

const rosidl_action_type_support_t * ActionTypes::action_handle() const
{
  return action_ts_;
}

const rosidl_action_type_support_t * ActionTypes::cpp_action_handle() const
{
  return cpp_action_ts_;
}

ActionTypes::Struct ActionTypes::make_struct(const MessageMembers *members) const
{
  Struct out;
  out.members = members;
  out.storage.resize(members->size_of_);
  members->init_function(out.storage.data(), rosidl_runtime_cpp::MessageInitialization::ALL);
  return out;
}  // GCOVR_EXCL_LINE: the close executes on every return; gcovr leaves it cold

ActionTypes::Struct ActionTypes::make_goal() const
{
  return make_struct(goal_members_);
}

ActionTypes::Struct ActionTypes::make_result() const
{
  return make_struct(result_members_);
}

ActionTypes::Struct ActionTypes::make_feedback() const
{
  return make_struct(feedback_members_);
}

ActionTypes::Struct ActionTypes::make_goal_request() const
{
  return make_struct(goal_request_members_);
}

ActionTypes::Struct ActionTypes::make_goal_response() const
{
  return make_struct(goal_response_members_);
}

ActionTypes::Struct ActionTypes::make_result_request() const
{
  return make_struct(result_request_members_);
}

ActionTypes::Struct ActionTypes::make_result_response() const
{
  return make_struct(result_response_members_);
}

ActionTypes::Struct ActionTypes::make_feedback_message() const
{
  return make_struct(feedback_message_members_);
}

ActionTypes::Struct ActionTypes::make_cancel_request() const
{
  return make_struct(cancel_request_members_);
}

ActionTypes::Struct ActionTypes::make_cancel_response() const
{
  return make_struct(cancel_response_members_);
}

bool ActionTypes::goal_id_of(
  const Struct & request, std::array<std::uint8_t, 16> & goal_id) const
{
  if (request.members == nullptr || request.storage.empty()) {
    return false;
  }
  return read_uuid(request.members, request.storage.data(), "goal_id", goal_id);
}

const void * ActionTypes::goal_of(const Struct & request) const
{
  if (request.members == nullptr || request.storage.empty()) {
    return nullptr;
  }
  return nested_storage(request.members, request.storage.data(), "goal");
}

bool ActionTypes::fill_goal_response(
  Struct & response, std::int32_t sec, std::uint32_t nanosec, bool accepted) const
{
  if (response.members == nullptr || response.storage.empty()) {
    return false;
  }
  return write_time(response.members, response.storage.data(), "stamp", sec, nanosec) &&
         write_bool(response.members, response.storage.data(), "accepted", accepted);
}

bool ActionTypes::result_id_of(
  const Struct & request, std::array<std::uint8_t, 16> & goal_id) const
{
  if (request.members == nullptr || request.storage.empty()) {
    return false;
  }
  return read_uuid(request.members, request.storage.data(), "goal_id", goal_id);
}

bool ActionTypes::fill_result_status(Struct & response, std::int8_t status) const
{
  if (response.members == nullptr || response.storage.empty()) {
    return false;
  }
  return write_int8(response.members, response.storage.data(), "status", status);
}

void * ActionTypes::result_of(Struct & response) const
{
  if (response.members == nullptr || response.storage.empty()) {
    return nullptr;
  }
  return nested_storage(response.members, response.storage.data(), "result");
}

bool ActionTypes::fill_feedback_id(
  Struct & message, const std::array<std::uint8_t, 16> & goal_id) const
{
  if (message.members == nullptr || message.storage.empty()) {
    return false;
  }
  return write_uuid(message.members, message.storage.data(), "goal_id", goal_id);
}

void * ActionTypes::feedback_of(Struct & message) const
{
  if (message.members == nullptr || message.storage.empty()) {
    return nullptr;
  }
  return nested_storage(message.members, message.storage.data(), "feedback");
}

bool ActionTypes::cancel_of(
  const Struct & request, std::array<std::uint8_t, 16> & goal_id, std::int32_t & stamp_sec,
  std::uint32_t & stamp_nanosec) const
{
  if (request.members == nullptr || request.storage.empty()) {
    return false;
  }
  const void *info = nested_storage(request.members, request.storage.data(), "goal_info");
  if (info == nullptr) {
    return false;
  }
  const MessageMember *member = find_member(request.members, "goal_info");
  const MessageMembers *info_members = nested_members(member);
  return read_uuid(info_members, info, "goal_id", goal_id) &&
         read_time(info_members, info, "stamp", stamp_sec, stamp_nanosec);
}

bool ActionTypes::fill_cancel_response(
  Struct & response, std::int8_t return_code, const std::array<std::uint8_t, 16> & goal_id,
  std::int32_t stamp_sec, std::uint32_t stamp_nanosec, bool canceling) const
{
  if (response.members == nullptr || response.storage.empty()) {
    return false;
  }
  if (!write_int8(response.members, response.storage.data(), "return_code", return_code)) {
    return false;
  }
  const MessageMember *member = find_member(response.members, "goals_canceling");
  // GCOVR_EXCL_START: reaching here means return_code filled, which only a
  // cancel response carries, and every cancel response carries the goals.
  if (member == nullptr || !member->is_array_ || member->resize_function == nullptr) {
    return false;
  }
  // GCOVR_EXCL_STOP
  const MessageMembers *info_members = nested_members(member);
  // GCOVR_EXCL_START: installed sequences always describe their members;
  // only a hand-built layout could miss them.
  if (info_members == nullptr) {
    return false;
  }
  // GCOVR_EXCL_STOP
  auto *base = static_cast<std::uint8_t *>(response.storage.data());
  member->resize_function(base + member->offset_, canceling ? 1u : 0u);
  if (!canceling) {
    return true;
  }
  // GCOVR_EXCL_START: installed sequences always fetch their members;
  // only a hand-built layout could miss the getter.
  if (member->get_function == nullptr) {
    return false;
  }
  // GCOVR_EXCL_STOP
  void *info = member->get_function(base + member->offset_, 0u);
  return write_uuid(info_members, info, "goal_id", goal_id) &&
         write_time(info_members, info, "stamp", stamp_sec, stamp_nanosec);
}

}  // namespace r2s_bridge
