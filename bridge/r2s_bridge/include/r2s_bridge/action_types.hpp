// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors
// The introspection handles behind one action type: the goal, result and
// feedback message members for struct allocation and conversion, the
// envelope members for the goal, result and cancel exchanges, and the
// action handle for the rcl action server (docs/00-design.md, P1 actions;
// research ticket R14).
#ifndef R2S_BRIDGE__ACTION_TYPES_HPP_
#define R2S_BRIDGE__ACTION_TYPES_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rcpputils/shared_library.hpp"
#include "rosidl_runtime_c/action_type_support_struct.h"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"

namespace r2s_bridge
{

/// Loads the introspection type support of one "package/action/Type".
/**
 * Keeps its libraries alive. Throws std::runtime_error naming the type when
 * the library, a message handle or the action handle does not load. Not
 * copyable; safe to share between threads once loaded.
 */
class ActionTypes
{
public:
  /// Loads one action type.
  /**
   * \param[in] type_name the "package/action/Type" to load.
   * \throws std::runtime_error naming the type when anything fails to load.
   */
  explicit ActionTypes(const std::string & type_name);
  /// Unloads the type support.
  ~ActionTypes();
  /// Moves the loaded handles.
  /**
   * \param[in] other the object to take over.
   */
  ActionTypes(ActionTypes && other) noexcept = default;
  /// Moves the loaded handles.
  /**
   * \param[in] other the object to take over.
   * \return A reference to this object.
   */
  ActionTypes & operator=(ActionTypes && other) noexcept = default;
  /// Not copyable.
  ActionTypes(const ActionTypes &) = delete;
  /// Not assignable.
  ActionTypes & operator=(const ActionTypes &) = delete;

  /// One owned introspection struct.
  /**
   * init_function ran at construction, fini_function runs at destruction.
   * Move-only, so one fini runs once.
   */
  struct Struct
  {
    /// The members the struct was initialized with.
    const rosidl_typesupport_introspection_cpp::MessageMembers *members = nullptr;
    /// The bytes init_function initialized.
    std::vector<std::uint8_t> storage;
    /// An empty struct, no members.
    Struct() = default;
    /// Moves the owned storage.
    /**
     * \param[in] other the object to take over.
     */
    Struct(Struct && other) noexcept;
    /// Moves the owned storage.
    /**
     * \param[in] other the object to take over.
     * \return A reference to this object.
     */
    Struct & operator=(Struct && other) noexcept;
    /// Not copyable.
    Struct(const Struct &) = delete;
    /// Not assignable.
    Struct & operator=(const Struct &) = delete;
    /// Runs fini_function on the owned storage.
    ~Struct();
  };

  /// The goal message type name the converter plans cache under.
  /**
   * \return The goal type name.
   */
  std::string goal_type_name() const;
  /// The result message type name the converter plans cache under.
  /**
   * \return The result type name.
   */
  std::string result_type_name() const;
  /// The feedback message type name the converter plans cache under.
  /**
   * \return The feedback type name.
   */
  std::string feedback_type_name() const;
  /// The first 8 bytes of the installed action type hash.
  /**
   * The walk compares them against the DECLARE ([ACT-7]).
   *
   * \return The installed hash prefix.
   */
  std::array<std::uint8_t, 8> type_hash() const;
  /// The introspection action handle.
  /**
   * For nothing the bridge sends through: rcl takes the C++ handle below,
   * while the structs it fills are introspection-initialized (the layouts
   * match, as the topic path shows).
   *
   * \return The action handle.
   */
  const rosidl_action_type_support_t * action_handle() const;
  /// The C++ action handle.
  /**
   * The one the rcl action server is created with.
   *
   * \return The C++ action handle.
   */
  const rosidl_action_type_support_t * cpp_action_handle() const;
  /// An initialized goal struct.
  /**
   * \return The goal struct.
   */
  Struct make_goal() const;
  /// An initialized result struct.
  /**
   * \return The result struct.
   */
  Struct make_result() const;
  /// An initialized feedback struct.
  /**
   * \return The feedback struct.
   */
  Struct make_feedback() const;
  /// An initialized SendGoal request struct.
  /**
   * What the rcl server takes a ROS goal request into.
   *
   * \return The goal request struct.
   */
  Struct make_goal_request() const;
  /// An initialized SendGoal response struct.
  /**
   * Answered with fill_goal_response before sending.
   *
   * \return The goal response struct.
   */
  Struct make_goal_response() const;
  /// An initialized GetResult request struct.
  /**
   * What the rcl server takes a ROS result request into.
   *
   * \return The result request struct.
   */
  Struct make_result_request() const;
  /// An initialized GetResult response struct.
  /**
   * The status set with fill_result_status, the result payload unpacked
   * into the nested result before sending.
   *
   * \return The result response struct.
   */
  Struct make_result_response() const;
  /// An initialized FeedbackMessage struct.
  /**
   * The goal id set with fill_feedback_id, the feedback payload unpacked
   * into the nested feedback before publishing.
   *
   * \return The feedback message struct.
   */
  Struct make_feedback_message() const;
  /// An initialized CancelGoal request struct.
  /**
   * What the rcl server takes a ROS cancel request into.
   *
   * \return The cancel request struct.
   */
  Struct make_cancel_request() const;
  /// An initialized CancelGoal response struct.
  /**
   * Answered with fill_cancel_response before sending.
   *
   * \return The cancel response struct.
   */
  Struct make_cancel_response() const;
  /// Reads the goal id out of a taken SendGoal request.
  /**
   * \param[in] request the taken request.
   * \param[out] goal_id the 16 request bytes.
   * \return False when the request has no goal id field.
   */
  bool goal_id_of(const Struct & request, std::array<std::uint8_t, 16> & goal_id) const;
  /// Reads the nested goal out of a taken SendGoal request.
  /**
   * The pointer stays valid while request lives; it names the storage the
   * converter packs from.
   *
   * \param[in] request the taken request.
   * \return The nested goal storage, or nullptr without a goal field.
   */
  const void * goal_of(const Struct & request) const;
  /// Writes the SendGoal response fields.
  /**
   * \param[in] response the response to fill.
   * \param[in] sec the stamp seconds.
   * \param[in] nanosec the stamp nanoseconds.
   * \param[in] accepted whether the MCU took the goal.
   * \return False when a field is missing.
   */
  bool fill_goal_response(
    Struct & response, std::int32_t sec, std::uint32_t nanosec, bool accepted) const;
  /// Reads the goal id out of a taken GetResult request.
  /**
   * \param[in] request the taken request.
   * \param[out] goal_id the 16 request bytes.
   * \return False when the request has no goal id field.
   */
  bool result_id_of(const Struct & request, std::array<std::uint8_t, 16> & goal_id) const;
  /// Writes the GetResult response status.
  /**
   * The nested result is unpacked by the caller before sending.
   *
   * \param[in] response the response to fill.
   * \param[in] status the terminal status code.
   * \return False when the status field is missing.
   */
  bool fill_result_status(Struct & response, std::int8_t status) const;
  /// Returns the nested result storage of a GetResult response.
  /**
   * The pointer stays valid while response lives; the caller unpacks the
   * wire result into it before sending.
   *
   * \param[in] response the response holding the result.
   * \return The nested result storage, or nullptr without a result field.
   */
  void * result_of(Struct & response) const;
  /// Writes the goal id of a FeedbackMessage.
  /**
   * The nested feedback is unpacked by the caller before publishing.
   *
   * \param[in] message the message to fill.
   * \param[in] goal_id the 16 goal bytes.
   * \return False when a field is missing.
   */
  bool fill_feedback_id(Struct & message, const std::array<std::uint8_t, 16> & goal_id) const;
  /// Returns the nested feedback storage of a FeedbackMessage.
  /**
   * The pointer stays valid while message lives; the caller unpacks the
   * wire feedback into it before publishing.
   *
   * \param[in] message the message holding the feedback.
   * \return The nested feedback storage, or nullptr without a feedback field.
   */
  void * feedback_of(Struct & message) const;
  /// Reads a taken CancelGoal request.
  /**
   * \param[in] request the taken request.
   * \param[out] goal_id the requested 16 goal bytes, zeroes for all goals.
   * \param[out] stamp_sec the requested stamp seconds.
   * \param[out] stamp_nanosec the requested stamp nanoseconds.
   * \return False when a field is missing.
   */
  bool cancel_of(
    const Struct & request, std::array<std::uint8_t, 16> & goal_id, std::int32_t & stamp_sec,
    std::uint32_t & stamp_nanosec) const;
  /// Writes a CancelGoal response.
  /**
   * \param[in] response the response to fill.
   * \param[in] return_code the cancel verdict for the ROS client.
   * \param[in] goal_id the canceled goal bytes, ignored when canceling nothing.
   * \param[in] stamp_sec the canceled goal stamp seconds.
   * \param[in] stamp_nanosec the canceled goal stamp nanoseconds.
   * \param[in] canceling whether a goal is listed as canceling.
   * \return False when a field is missing.
   */
  bool fill_cancel_response(
    Struct & response, std::int8_t return_code, const std::array<std::uint8_t, 16> & goal_id,
    std::int32_t stamp_sec, std::uint32_t stamp_nanosec, bool canceling) const;

private:
  std::string type_name_;
  std::shared_ptr<rcpputils::SharedLibrary> library_;
  std::shared_ptr<rcpputils::SharedLibrary> cpp_library_;
  std::shared_ptr<rcpputils::SharedLibrary> cancel_library_;
  const rosidl_action_type_support_t *cpp_action_ts_ = nullptr;
  const rosidl_action_type_support_t *action_ts_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *goal_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *result_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *feedback_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *goal_request_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *goal_response_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *result_request_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *result_response_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *feedback_message_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *cancel_request_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *cancel_response_members_ = nullptr;

  Struct make_struct(
    const rosidl_typesupport_introspection_cpp::MessageMembers *members) const;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__ACTION_TYPES_HPP_
