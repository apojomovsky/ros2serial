// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

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

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__ROS_CLOCK_HPP_
