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

} // namespace
} // namespace test
} // namespace r2s_bridge
