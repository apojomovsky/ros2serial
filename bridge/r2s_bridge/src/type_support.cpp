// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The topic verdict: type support loading and hash check, through the
// installed rosidl_typesupport_cpp library.
#include "r2s_bridge/type_support.hpp"

#include <algorithm>
#include <exception>
#include <memory>

#include "r2s_bridge/action_types.hpp"
#include "rclcpp/typesupport_helpers.hpp"
#include "rcpputils/shared_library.hpp"

namespace r2s_bridge
{

std::uint8_t resolve_topic_type(const TopicInfo & info)
{
  try {
        // The library stays loaded while the handle is read below; the H6
        // converter keeps its own libraries for the plans it caches.
    const std::shared_ptr<rcpputils::SharedLibrary> library =
      rclcpp::get_typesupport_library(info.type_name, "rosidl_typesupport_cpp");
        // Both calls throw when the type does not load, so a null check
        // would never fire; the catch below is the only error path.
    const rosidl_message_type_support_t *handle =
      rclcpp::get_message_typesupport_handle(info.type_name, "rosidl_typesupport_cpp", *library);
    const rosidl_type_hash_t *installed = handle->get_type_hash_func(handle);
        // Only the 8 wire bytes decide ([CTL-12]); the other 24 are ignored.
    const bool matches =
      std::equal(info.type_hash.begin(), info.type_hash.end(), std::begin(installed->value));
    return matches ? kTopicStatusOk : kTopicStatusHashMismatch;
  } catch (const std::exception &) {
        // A malformed name, a missing package and an unloadable library all
        // refuse the topic the same way ([CTL-13]).
    return kTopicStatusUnknownType;
  }
}

std::uint8_t resolve_service_type(const ServiceInfo & info)
{
  try {
    const std::shared_ptr<rcpputils::SharedLibrary> library =
      rclcpp::get_typesupport_library(info.type_name, "rosidl_typesupport_cpp");
    const rosidl_service_type_support_t *handle =
      rclcpp::get_service_typesupport_handle(info.type_name, "rosidl_typesupport_cpp", *library);
    const rosidl_type_hash_t *installed = handle->get_type_hash_func(handle);
        // Only the 8 wire bytes decide ([CTL-12]); the other 24 are ignored.
    const bool matches =
      std::equal(info.type_hash.begin(), info.type_hash.end(), std::begin(installed->value));
    return matches ? kServiceStatusOk : kServiceStatusHashMismatch;
  } catch (const std::exception &) {
        // A malformed name, a missing package and an unloadable library all
        // refuse the service the same way ([SVR-7]).
    return kServiceStatusUnknownType;
  }
}

std::uint8_t resolve_action_type(const ActionInfo & info)
{
  try {
    const ActionTypes types(info.type_name);
    const std::array<std::uint8_t, 8> installed = types.type_hash();
        // Only the 8 wire bytes decide ([CTL-12]); the other 24 are ignored.
    const bool matches = std::equal(
      info.type_hash.begin(), info.type_hash.end(), installed.begin());
    return matches ? kActionStatusOk : kActionStatusHashMismatch;
  // Catch body runs (unknown-type test); gcov miscounts the taken arc.
  } catch (const std::exception &) {  // GCOV_EXCL_BR_LINE
        // A malformed name, a missing package and an unloadable library all
        // refuse the action the same way ([ACT-7]).
    return kActionStatusUnknownType;
  }
}

}  // namespace r2s_bridge
