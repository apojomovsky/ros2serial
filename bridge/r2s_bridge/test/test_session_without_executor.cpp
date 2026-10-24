#include <gtest/gtest.h>

#include <rclcpp/rclcpp.hpp>

#include <chrono>
#include <cstdint>
#include <vector>

#include "fake_transport.hpp"
#include "manual_clock.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

// A session test owns the loop: it reads the link, answers it and decides when
// time passes. No executor runs anything, and the manual clock is the only time
// source, so the exchange below is exact.
TEST(SessionWithoutExecutor, RunsAScriptedExchangeOnTheManualClock)
{
    ASSERT_FALSE(rclcpp::ok()) << "a session test must not need an rclcpp context";

    FakeTransport transport;
    ManualClock clock;
    transport.script_read({0x11, 0x22});
    transport.script_read(IoStatus::kWouldBlock);
    transport.script_read({0x33});

    std::vector<std::uint8_t> received;
    std::uint8_t buffer[8]{};
    for (int pass = 0; pass < 3; ++pass) {
        const IoResult got = transport.read(buffer, sizeof buffer);
        if (got.status == IoStatus::kOk) {
            received.insert(received.end(), buffer, buffer + got.bytes);
            transport.write(buffer, got.bytes);
        } else if (got.status == IoStatus::kWouldBlock) {
            clock.advance(std::chrono::milliseconds(5));
        }
    }

    EXPECT_EQ(received, (std::vector<std::uint8_t>{0x11, 0x22, 0x33}));
    EXPECT_EQ(transport.writes(), (std::vector<std::vector<std::uint8_t>>{{0x11, 0x22}, {0x33}}));
    EXPECT_EQ(clock.now(), std::chrono::milliseconds(5));
}

} // namespace
} // namespace test
} // namespace r2s_bridge
