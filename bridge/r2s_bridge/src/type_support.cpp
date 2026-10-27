// The topic verdict: type support loading and hash check, through the
// installed rosidl_typesupport_cpp library.
#include "r2s_bridge/type_support.hpp"

#include <algorithm>
#include <exception>
#include <memory>

#include "rclcpp/typesupport_helpers.hpp"
#include "rcpputils/shared_library.hpp"

namespace r2s_bridge
{

std::uint8_t resolve_topic_type(const TopicInfo &info)
{
    try {
        // The library stays loaded while the handle is read below; the H6
        // converter keeps its own libraries for the plans it caches.
        const std::shared_ptr<rcpputils::SharedLibrary> library =
            rclcpp::get_typesupport_library(info.type_name, "rosidl_typesupport_cpp");
        const rosidl_message_type_support_t *handle =
            rclcpp::get_message_typesupport_handle(info.type_name, "rosidl_typesupport_cpp", *library);
        if (handle == nullptr || handle->get_type_hash_func == nullptr) {
            return kTopicStatusUnknownType;
        }
        const rosidl_type_hash_t *installed = handle->get_type_hash_func(handle);
        if (installed == nullptr) {
            return kTopicStatusUnknownType;
        }
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

} // namespace r2s_bridge
