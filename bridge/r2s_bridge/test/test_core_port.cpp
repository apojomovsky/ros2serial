// The core port with one slot per live link: reads and ticks come from the
// slot's own transport and clock, a fault marks only its own slot, and
// attaching past the table throws instead of sharing a link.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "core_port.hpp"
#include "fake_transport.hpp"
#include "manual_clock.hpp"

extern "C" {
#include "r2s/r2s_port.h"
}

namespace r2s_bridge
{
namespace test
{
namespace
{

TEST(CorePort, ReadsEmptyAndTicksZeroWithoutASlot)
{
    std::uint8_t buffer[4]{0xAA, 0xBB, 0xCC, 0xDD};

    EXPECT_EQ(r2s_port_read_link(3u, buffer, sizeof buffer), 0u);
    EXPECT_EQ(buffer[0], 0xAAu) << "no byte is written without a transport";
    EXPECT_FALSE(link_faulted(3u)) << "an empty read is not a link fault";
    EXPECT_EQ(r2s_port_tick_link(3u), 0u);
}

TEST(CorePort, AttachHandsOutOneSlotPerPair)
{
    FakeTransport first_transport;
    FakeTransport second_transport;
    ManualClock first_clock;
    ManualClock second_clock;

    const std::uint8_t first = attach_core_port(first_transport, first_clock);
    const std::uint8_t second = attach_core_port(second_transport, second_clock);
    EXPECT_NE(first, second);

    detach_core_port(first);
    detach_core_port(second);
}

TEST(CorePort, DetachFreesTheSlotForReuse)
{
    FakeTransport transport;
    ManualClock clock;

    const std::uint8_t link = attach_core_port(transport, clock);
    detach_core_port(link);
    // Attaching again after the detach must not throw: the slot is free.
    detach_core_port(attach_core_port(transport, clock));
}

TEST(CorePort, AttachPastACapacityTableThrows)
{
    FakeTransport transport;
    ManualClock clock;
    std::vector<std::uint8_t> links;

    for (std::uint8_t n = 0; n < R2S_MAX_LINKS; n++) {
    links.push_back(attach_core_port(transport, clock));
    }
    EXPECT_THROW(attach_core_port(transport, clock), std::logic_error);

    for (const std::uint8_t link : links) {
    detach_core_port(link);
    }
}

TEST(CorePort, AFaultMarksOnlyItsOwnSlot)
{
    FakeTransport first_transport;
    FakeTransport second_transport;
    ManualClock clock;
    std::uint8_t buffer[4]{};

    const std::uint8_t first = attach_core_port(first_transport, clock);
    const std::uint8_t second = attach_core_port(second_transport, clock);

    first_transport.close();
    EXPECT_EQ(r2s_port_read_link(first, buffer, sizeof buffer), 0u);
    EXPECT_TRUE(link_faulted(first));
    EXPECT_FALSE(link_faulted(second));

    clear_link_fault(first);
    EXPECT_FALSE(link_faulted(first));

    detach_core_port(first);
    detach_core_port(second);
}

TEST(CorePort, PlainSeamReachesTheDefaultLink)
{
    // The plain seam is the singleton's link. An empty table hands the first
    // attach slot 0, so the plain functions below reach that slot's pair.
    FakeTransport transport;
    ManualClock clock(std::chrono::milliseconds(40));
    std::vector<std::uint8_t> links;

    for (std::uint8_t n = 0; n < R2S_MAX_LINKS; n++) {
    links.push_back(attach_core_port(transport, clock));
    }
    for (const std::uint8_t link : links) {
    detach_core_port(link);
    }
    const std::uint8_t link = attach_core_port(transport, clock);
    EXPECT_EQ(link, 0u);

    transport.script_read(std::vector<std::uint8_t>{0x11, 0x22});
    std::uint8_t buffer[4]{0xAA, 0xBB, 0xCC, 0xDD};
    EXPECT_EQ(r2s_port_read(buffer, sizeof buffer), 2u);
    EXPECT_EQ(buffer[0u], 0x11u);
    EXPECT_EQ(buffer[1u], 0x22u);
    EXPECT_EQ(r2s_port_tick(), 40u);

    detach_core_port(link);
}

TEST(CorePort, EachSlotTicksItsOwnClock)
{
    FakeTransport first_transport;
    FakeTransport second_transport;
    ManualClock first_clock(std::chrono::milliseconds(40));
    ManualClock second_clock(std::chrono::milliseconds(90));

    const std::uint8_t first = attach_core_port(first_transport, first_clock);
    const std::uint8_t second = attach_core_port(second_transport, second_clock);

    EXPECT_EQ(r2s_port_tick_link(first), 40u);
    EXPECT_EQ(r2s_port_tick_link(second), 90u);

    detach_core_port(first);
    detach_core_port(second);
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
