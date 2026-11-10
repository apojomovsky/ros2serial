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

// A Clock over a ROS-time callback, so the session stamps corrections in ROS
// time while measuring round trips on the steady clock.
#include "r2s_bridge/ros_clock.hpp"

#include <stdexcept>
#include <utility>

namespace r2s_bridge
{

RosClock::RosClock(std::function<std::int64_t()> now_ns)
: now_ns_(std::move(now_ns))
{
  if (!now_ns_) {
    throw std::invalid_argument("r2s_bridge::RosClock: a time source is required");
  }
}

std::chrono::nanoseconds RosClock::now() const
{
  return std::chrono::nanoseconds(now_ns_());
}

}  // namespace r2s_bridge
