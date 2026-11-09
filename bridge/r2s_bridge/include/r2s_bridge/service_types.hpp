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

// Loads the introspection type support of one "package/srv/Type" and keeps
// its library alive. Throws std::runtime_error naming the type when the
// library, the request handle, the response handle or the service handle
// does not load. Not copyable; safe to share between threads once loaded.
class ServiceTypes
{
public:
  explicit ServiceTypes(const std::string & type_name);
  ~ServiceTypes();
  ServiceTypes(ServiceTypes && other) noexcept = default;
  ServiceTypes & operator=(ServiceTypes && other) noexcept = default;
  ServiceTypes(const ServiceTypes &) = delete;
  ServiceTypes & operator=(const ServiceTypes &) = delete;

    // One owned introspection struct: init_function ran at construction,
    // fini_function runs at destruction. Move-only, so one fini runs once.
  struct Struct
  {
    const rosidl_typesupport_introspection_cpp::MessageMembers *members = nullptr;
    std::vector<std::uint8_t> storage;
    Struct() = default;
    Struct(Struct && other) noexcept;
    Struct & operator=(Struct && other) noexcept;
    Struct(const Struct &) = delete;
    Struct & operator=(const Struct &) = delete;
    ~Struct();
  };

    // The message type names the converter plans cache under.
  std::string request_type_name() const;
  std::string response_type_name() const;
    // The introspection service handle, for the rcl service server on
    // distros without a generic service API.
  const rosidl_service_type_support_t * service_handle() const;
    // The C++ service handle: the one the middleware takes and sends
    // through, while the structs it fills are introspection-initialized
    // (the layouts match, as the topic path shows).
  const rosidl_service_type_support_t * cpp_service_handle() const;
  std::size_t request_size() const;
  std::size_t response_size() const;
  Struct make_request() const;
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
