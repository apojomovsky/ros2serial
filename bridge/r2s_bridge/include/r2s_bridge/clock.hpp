// The bridge's only time source: monotonic host time in production, a clock the
// test moves by hand. Nothing in the session reads a clock directly, so a test
// decides exactly when a deadline expires.
#ifndef R2S_BRIDGE__CLOCK_HPP_
#define R2S_BRIDGE__CLOCK_HPP_

#include <chrono>

namespace r2s_bridge
{

class Clock
{
  public:
    virtual ~Clock() = default;

    // Time since a fixed, arbitrary origin, in nanoseconds. Successive calls on
    // one instance never go backwards.
    virtual std::chrono::nanoseconds now() const = 0;
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__CLOCK_HPP_
