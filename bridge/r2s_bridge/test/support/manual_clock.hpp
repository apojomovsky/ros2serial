// A clock the test moves: time passes only through advance(), never with the
// wall clock, so a timeout test fixes exactly when its deadline expires.
#ifndef R2S_BRIDGE__MANUAL_CLOCK_HPP_
#define R2S_BRIDGE__MANUAL_CLOCK_HPP_

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

} // namespace test
} // namespace r2s_bridge

#endif // R2S_BRIDGE__MANUAL_CLOCK_HPP_
