// The bridge's ROS-time source: the node's clock for the CORRECTION stamps.
// Steady time measures the round trip; ROS time stamps it (see time_sync.hpp).
#ifndef R2S_BRIDGE__ROS_CLOCK_HPP_
#define R2S_BRIDGE__ROS_CLOCK_HPP_

#include <chrono>
#include <cstdint>
#include <functional>

#include "r2s_bridge/clock.hpp"

namespace r2s_bridge
{

// A Clock over ROS time in nanoseconds since the ROS epoch. The bridge binds
// it to its node clock; tests bind it to a second manual clock.
class RosClock final : public Clock
{
  public:
    // Throws std::invalid_argument without a source to read.
    explicit RosClock(std::function<std::int64_t()> now_ns);
    std::chrono::nanoseconds now() const override;

  private:
    std::function<std::int64_t()> now_ns_;
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__ROS_CLOCK_HPP_
