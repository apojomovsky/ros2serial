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

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>

#include "manual_clock.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

TEST(ManualClock, StartsAtZero)
{
    ManualClock clock;
    EXPECT_EQ(clock.now(), std::chrono::nanoseconds::zero());
}

TEST(ManualClock, StartsAtTheGivenTime)
{
    ManualClock clock(std::chrono::seconds(5));
    EXPECT_EQ(clock.now(), std::chrono::seconds(5));
}

TEST(ManualClock, DoesNotMoveOnItsOwn)
{
    ManualClock clock;
    const auto start = clock.now();
    for (int read = 0; read < 3; ++read) {
    EXPECT_EQ(clock.now(), start);
    }
}

TEST(ManualClock, AdvancesAccumulate)
{
    ManualClock clock;
    clock.advance(std::chrono::milliseconds(250));
    clock.advance(std::chrono::milliseconds(750));
    EXPECT_EQ(clock.now(), std::chrono::seconds(1));
}

TEST(ManualClock, RejectsABackwardsAdvance)
{
    ManualClock clock(std::chrono::seconds(5));
    EXPECT_THROW(clock.advance(std::chrono::nanoseconds(-1)), std::invalid_argument);
    EXPECT_EQ(clock.now(), std::chrono::seconds(5));
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
