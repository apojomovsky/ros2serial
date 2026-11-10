// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors
// The introspection handles behind one service type, loaded by name.
#include "r2s_bridge/service_types.hpp"

#include <stdexcept>

#include "rclcpp/typesupport_helpers.hpp"
#include "rosidl_runtime_cpp/message_initialization.hpp"

namespace r2s_bridge
{

namespace
{

constexpr const char *kIntrospectionId = "rosidl_typesupport_introspection_cpp";

}  // namespace

ServiceTypes::Struct::Struct(Struct && other) noexcept
: members(other.members), storage(std::move(other.storage))
{
  other.members = nullptr;
}

ServiceTypes::Struct & ServiceTypes::Struct::operator=(Struct && other) noexcept
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

ServiceTypes::Struct::~Struct()
{
  if (members != nullptr && !storage.empty()) {
    members->fini_function(storage.data());
  }
}

ServiceTypes::ServiceTypes(const std::string & type_name)
: type_name_(type_name)
{
  try {
    constexpr const char * kCppId = "rosidl_typesupport_cpp";
    cpp_library_ = rclcpp::get_typesupport_library(type_name, kCppId);
    cpp_service_ts_ = rclcpp::get_service_typesupport_handle(type_name, kCppId, *cpp_library_);
    library_ = rclcpp::get_typesupport_library(type_name, kIntrospectionId);
    const rosidl_message_type_support_t *request_ts =
      rclcpp::get_message_typesupport_handle(type_name + "_Request", kIntrospectionId, *library_);
    const rosidl_message_type_support_t *response_ts =
      rclcpp::get_message_typesupport_handle(type_name + "_Response", kIntrospectionId, *library_);
    service_ts_ =
      rclcpp::get_service_typesupport_handle(type_name, kIntrospectionId, *library_);
    request_members_ =
      static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(request_ts->data);
    response_members_ =
      static_cast<const rosidl_typesupport_introspection_cpp::MessageMembers *>(response_ts->data);
  } catch (const std::exception & failure) {
    throw std::runtime_error("service '" + type_name + "': no introspection type support (" +
      failure.what() + ")");
  }
  // GCOVR_EXCL_START: the lookups above throw when a symbol is missing,
  // so a null without a throw would break their documented contract, and
  // the handles are read into the members first; a unit test cannot stage
  // the state these guards describe.
  if (request_members_ == nullptr || response_members_ == nullptr || service_ts_ == nullptr) {
    throw std::runtime_error("service '" + type_name + "': no introspection type support");
  }
  if (request_members_->init_function == nullptr || request_members_->fini_function == nullptr ||
    response_members_->init_function == nullptr || response_members_->fini_function == nullptr)
  {
    throw std::runtime_error("service '" + type_name + "': no init or fini function");
  }
  // GCOVR_EXCL_STOP
}

ServiceTypes::~ServiceTypes()
{
  // The members point into the library, which outlives this object.
}

std::string ServiceTypes::request_type_name() const
{
  return type_name_ + "_Request";
}

std::string ServiceTypes::response_type_name() const
{
  return type_name_ + "_Response";
}

const rosidl_service_type_support_t * ServiceTypes::service_handle() const
{
  return service_ts_;
}

const rosidl_service_type_support_t * ServiceTypes::cpp_service_handle() const
{
  return cpp_service_ts_;
}

std::size_t ServiceTypes::request_size() const
{
  return request_members_->size_of_;
}

std::size_t ServiceTypes::response_size() const
{
  return response_members_->size_of_;
}

ServiceTypes::Struct ServiceTypes::make_request() const
{
  Struct out;
  out.members = request_members_;
  out.storage.resize(request_members_->size_of_);
  request_members_->init_function(
    out.storage.data(), rosidl_runtime_cpp::MessageInitialization::ALL);
  return out;
}

ServiceTypes::Struct ServiceTypes::make_response() const
{
  Struct out;
  out.members = response_members_;
  out.storage.resize(response_members_->size_of_);
  response_members_->init_function(
    out.storage.data(), rosidl_runtime_cpp::MessageInitialization::ALL);
  return out;
}

}  // namespace r2s_bridge
