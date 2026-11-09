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

// A clock the test moves: time passes only through advance(), never with the
// wall clock, so a timeout test fixes exactly when its deadline expires.
#ifndef SUPPORT__MANUAL_CLOCK_HPP_
#define SUPPORT__MANUAL_CLOCK_HPP_

#include <chrono>

#include "r2s_bridge/clock.hpp"

namespace r2s_bridge
{
namespace test
{

class ManualClock final : public Clock
{
public:
  explicit ManualClock(std::chrono::nanoseconds start = std::chrono::nanoseconds::zero());

    // Moves time forward. A negative delta is a test bug: it throws
    // std::invalid_argument and leaves now() unchanged.
  void advance(std::chrono::nanoseconds delta);

  std::chrono::nanoseconds now() const override;

private:
  std::chrono::nanoseconds now_;
};

}  // namespace test
}  // namespace r2s_bridge

#endif  // SUPPORT__MANUAL_CLOCK_HPP_
