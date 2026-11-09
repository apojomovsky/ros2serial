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

// The ROS clock: a time source in, ROS stamps out, with the missing source
// refused up front.
#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>

#include "r2s_bridge/ros_clock.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

TEST(RosClock, RejectsAMissingTimeSource)
{
    EXPECT_THROW(RosClock(std::function<std::int64_t()>()), std::invalid_argument);
}

TEST(RosClock, PropagatesASourceFailure)
{
    RosClock clock([]() -> std::int64_t {throw std::runtime_error("the source is gone");});

    EXPECT_THROW((void)clock.now(), std::runtime_error);
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
