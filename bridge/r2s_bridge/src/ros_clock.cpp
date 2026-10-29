// A Clock over a ROS-time callback, so the session stamps corrections in ROS
// time while measuring round trips on the steady clock.
#include "r2s_bridge/ros_clock.hpp"

#include <stdexcept>
#include <utility>

namespace r2s_bridge
{

RosClock::RosClock(std::function<std::int64_t()> now_ns) : now_ns_(std::move(now_ns))
{
    if (!now_ns_) {
        throw std::invalid_argument("r2s_bridge::RosClock: a time source is required");
    }
}

std::chrono::nanoseconds RosClock::now() const
{
    return std::chrono::nanoseconds(now_ns_());
}

} // namespace r2s_bridge
