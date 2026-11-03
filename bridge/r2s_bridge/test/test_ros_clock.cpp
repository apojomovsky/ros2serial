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
    RosClock clock([]() -> std::int64_t { throw std::runtime_error("the source is gone"); });

    EXPECT_THROW((void)clock.now(), std::runtime_error);
}

} // namespace
} // namespace test
} // namespace r2s_bridge
