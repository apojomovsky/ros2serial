#include "manual_clock.hpp"

#include <stdexcept>

namespace r2s_bridge
{
namespace test
{

ManualClock::ManualClock(std::chrono::nanoseconds start) : now_(start)
{
}

void ManualClock::advance(std::chrono::nanoseconds delta)
{
    if (delta < std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("ManualClock::advance needs a non-negative delta");
    }
    now_ += delta;
}

std::chrono::nanoseconds ManualClock::now() const
{
    return now_;
}

} // namespace test
} // namespace r2s_bridge
