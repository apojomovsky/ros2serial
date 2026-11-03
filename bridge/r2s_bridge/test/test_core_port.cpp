// The core port without an installed pair: reads are empty and the tick is
// zero, so the framer idles until a FrameReader attaches.
#include <gtest/gtest.h>

#include <cstdint>

#include "core_port.hpp"

extern "C" {
#include "r2s/r2s_port.h"
}

namespace r2s_bridge
{
namespace test
{
namespace
{

TEST(CorePort, ReadsEmptyWithoutATransport)
{
    detach_core_port();
    std::uint8_t buffer[4]{0xAA, 0xBB, 0xCC, 0xDD};

    EXPECT_EQ(r2s_port_read(buffer, sizeof buffer), 0u);
    EXPECT_EQ(buffer[0], 0xAAu) << "no byte is written without a transport";
    EXPECT_FALSE(link_faulted()) << "an empty read is not a link fault";
}

TEST(CorePort, TicksZeroWithoutAClock)
{
    detach_core_port();

    EXPECT_EQ(r2s_port_tick(), 0u);
}

} // namespace
} // namespace test
} // namespace r2s_bridge
