#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "fake_transport.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

TEST(FakeTransport, ReportsWouldBlockWhenNothingIsScripted)
{
    FakeTransport transport;
    std::uint8_t buffer[4]{};
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kWouldBlock}));
}

TEST(FakeTransport, ReturnsScriptedChunksInOrder)
{
    FakeTransport transport;
    transport.script_read({0x01, 0x02, 0x03});
    transport.script_read({0x04, 0x05});
    std::uint8_t buffer[8]{};

    const IoResult first = transport.read(buffer, sizeof buffer);
    ASSERT_EQ(first, (IoResult{3, IoStatus::kOk}));
    EXPECT_EQ(std::vector<std::uint8_t>(buffer, buffer + first.bytes), (std::vector<std::uint8_t>{1, 2, 3}));

    const IoResult second = transport.read(buffer, sizeof buffer);
    ASSERT_EQ(second, (IoResult{2, IoStatus::kOk}));
    EXPECT_EQ(std::vector<std::uint8_t>(buffer, buffer + second.bytes), (std::vector<std::uint8_t>{4, 5}));

    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kWouldBlock}));
}

TEST(FakeTransport, SplitsAChunkAtTheReadCapacity)
{
    FakeTransport transport;
    transport.script_read({0x01, 0x02, 0x03, 0x04, 0x05});
    std::uint8_t buffer[2]{};

    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{2, IoStatus::kOk}));
    EXPECT_EQ(std::vector<std::uint8_t>(buffer, buffer + 2), (std::vector<std::uint8_t>{1, 2}));
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{2, IoStatus::kOk}));
    EXPECT_EQ(std::vector<std::uint8_t>(buffer, buffer + 2), (std::vector<std::uint8_t>{3, 4}));
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{1, IoStatus::kOk}));
    EXPECT_EQ(buffer[0], 0x05);
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kWouldBlock}));
}

TEST(FakeTransport, ReportsScriptedStatusesInOrder)
{
    FakeTransport transport;
    transport.script_read({0x01});
    transport.script_read(IoStatus::kError);
    transport.script_read({0x02});
    std::uint8_t buffer[1]{};

    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{1, IoStatus::kOk}));
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kError}));
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{1, IoStatus::kOk}));
}

TEST(FakeTransport, ReportsAScriptedClose)
{
    FakeTransport transport;
    transport.script_read(IoStatus::kClosed);
    std::uint8_t buffer[1]{};
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kClosed}));
}

TEST(FakeTransport, RejectsAnEmptyChunk)
{
    FakeTransport transport;
    EXPECT_THROW(transport.script_read(std::vector<std::uint8_t>{}), std::invalid_argument);
}

TEST(FakeTransport, RejectsAnOkStatusWithoutBytes)
{
    FakeTransport transport;
    EXPECT_THROW(transport.script_read(IoStatus::kOk), std::invalid_argument);
}

TEST(FakeTransport, CapturesEveryWriteCall)
{
    FakeTransport transport;
    const std::vector<std::uint8_t> first{0xaa, 0xbb};
    const std::vector<std::uint8_t> second{0xcc};

    EXPECT_EQ(transport.write(first.data(), first.size()), (IoResult{2, IoStatus::kOk}));
    EXPECT_EQ(transport.write(second.data(), second.size()), (IoResult{1, IoStatus::kOk}));

    EXPECT_EQ(transport.writes(), (std::vector<std::vector<std::uint8_t>>{first, second}));
}

TEST(FakeTransport, CapsEachWriteAtTheLimit)
{
    FakeTransport transport;
    transport.limit_write(2);
    const std::vector<std::uint8_t> bytes{0x01, 0x02, 0x03, 0x04, 0x05};

    EXPECT_EQ(transport.write(bytes.data(), bytes.size()), (IoResult{2, IoStatus::kOk}));
    EXPECT_EQ(transport.write(bytes.data() + 2, 3), (IoResult{2, IoStatus::kOk}));
    EXPECT_EQ(transport.write(bytes.data() + 4, 1), (IoResult{1, IoStatus::kOk}));

    EXPECT_EQ(transport.writes(),
              (std::vector<std::vector<std::uint8_t>>{{0x01, 0x02}, {0x03, 0x04}, {0x05}}));
}

TEST(FakeTransport, ReportsWouldBlockWhenTheWriteLimitIsZero)
{
    FakeTransport transport;
    transport.limit_write(0);
    const std::uint8_t byte = 0x01;

    EXPECT_EQ(transport.write(&byte, 1), (IoResult{0, IoStatus::kWouldBlock}));
    EXPECT_TRUE(transport.writes().empty());
}

TEST(FakeTransport, RejectsAZeroLengthWrite)
{
    FakeTransport transport;
    const std::uint8_t byte = 0x01;
    EXPECT_THROW(transport.write(&byte, 0), std::invalid_argument);
}

TEST(FakeTransport, RejectsReadsAndWritesAfterClose)
{
    FakeTransport transport;
    transport.script_read({0x01});
    const std::uint8_t byte = 0x02;
    std::uint8_t buffer[1]{};

    transport.close();

    EXPECT_TRUE(transport.closed());
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kClosed}));
    EXPECT_EQ(transport.write(&byte, 1), (IoResult{0, IoStatus::kClosed}));
    EXPECT_TRUE(transport.writes().empty());
}

} // namespace
} // namespace test
} // namespace r2s_bridge
