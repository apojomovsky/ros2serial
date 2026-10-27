// The production clock: monotonic host time for the session and the pacer.
#ifndef R2S_BRIDGE__STEADY_CLOCK_HPP_
#define R2S_BRIDGE__STEADY_CLOCK_HPP_

#include <chrono>

#include "r2s_bridge/clock.hpp"

namespace r2s_bridge
{

class SteadyClock final : public Clock
{
  public:
    std::chrono::nanoseconds now() const override
    {
        return std::chrono::steady_clock::now().time_since_epoch();
    }
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__STEADY_CLOCK_HPP_
