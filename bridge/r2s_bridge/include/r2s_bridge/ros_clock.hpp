// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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

/// A Clock over ROS time in nanoseconds since the ROS epoch.
/**
 * The bridge binds it to its node clock; tests bind it to a second manual
 * clock.
 */
class RosClock final : public Clock
{
public:
  /// Reads ROS time through a source.
  /**
   * \param[in] now_ns the source to read.
   * \throws std::invalid_argument without a source to read.
   */
  explicit RosClock(std::function<std::int64_t()> now_ns);

  /// Time since the ROS epoch, in nanoseconds.
  /**
   * \return The source reading at the call.
   */
  std::chrono::nanoseconds now() const override;

private:
  std::function<std::int64_t()> now_ns_;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__ROS_CLOCK_HPP_
