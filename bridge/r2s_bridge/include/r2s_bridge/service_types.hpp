// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors
// The introspection handles behind one service type: the request and
// response message members for struct allocation and conversion, and the
// service handle for the rcl service server on distros without a generic
// service API (docs/00-design.md, P1 services; research ticket R4).
#ifndef R2S_BRIDGE__SERVICE_TYPES_HPP_
#define R2S_BRIDGE__SERVICE_TYPES_HPP_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "rcpputils/shared_library.hpp"
#include "rosidl_runtime_c/service_type_support_struct.h"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"

namespace r2s_bridge
{

/// Loads the introspection type support of one "package/srv/Type".
/**
 * Keeps its library alive. Throws std::runtime_error naming the type when
 * the library, the request handle, the response handle or the service
 * handle does not load. Not copyable; safe to share between threads once
 * loaded.
 */
class ServiceTypes
{
public:
  /// Loads one service type.
  /**
   * \param[in] type_name the "package/srv/Type" to load.
   * \throws std::runtime_error naming the type when anything fails to load.
   */
  explicit ServiceTypes(const std::string & type_name);
  /// Unloads the type support.
  ~ServiceTypes();
  /// Moves the loaded handles.
  /**
   * \param[in] other the object to take over.
   */
  ServiceTypes(ServiceTypes && other) noexcept = default;
  /// Moves the loaded handles.
  /**
   * \param[in] other the object to take over.
   * \return A reference to this object.
   */
  ServiceTypes & operator=(ServiceTypes && other) noexcept = default;
  /// Not copyable.
  ServiceTypes(const ServiceTypes &) = delete;
  /// Not assignable.
  ServiceTypes & operator=(const ServiceTypes &) = delete;

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

  /// The request message type name the converter plans cache under.
  /**
   * \return The request type name.
   */
  std::string request_type_name() const;
  /// The response message type name the converter plans cache under.
  /**
   * \return The response type name.
   */
  std::string response_type_name() const;
  /// The introspection service handle.
  /**
   * For the rcl service server on distros without a generic service API.
   *
   * \return The service handle.
   */
  const rosidl_service_type_support_t * service_handle() const;
  /// The C++ service handle.
  /**
   * The one the middleware takes and sends through, while the structs it
   * fills are introspection-initialized (the layouts match, as the topic
   * path shows).
   *
   * \return The C++ service handle.
   */
  const rosidl_service_type_support_t * cpp_service_handle() const;
  /// The request struct size in bytes.
  /**
   * \return The request size.
   */
  std::size_t request_size() const;
  /// The response struct size in bytes.
  /**
   * \return The response size.
   */
  std::size_t response_size() const;
  /// An initialized request struct.
  /**
   * \return The request struct.
   */
  Struct make_request() const;
  /// An initialized response struct.
  /**
   * \return The response struct.
   */
  Struct make_response() const;

private:
  std::string type_name_;
  std::shared_ptr<rcpputils::SharedLibrary> library_;
  std::shared_ptr<rcpputils::SharedLibrary> cpp_library_;
  const rosidl_service_type_support_t *cpp_service_ts_ = nullptr;
  const rosidl_service_type_support_t *service_ts_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *request_members_ = nullptr;
  const rosidl_typesupport_introspection_cpp::MessageMembers *response_members_ = nullptr;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__SERVICE_TYPES_HPP_
