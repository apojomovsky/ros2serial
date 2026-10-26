// The frame vectors through the bridge's own framing: the core framer reads
// each vector's wire from a scripted transport and must hand back its frame.
// The bridge also takes exactly the host's maximum decoded frame.
// covers: FRM-10
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "fake_transport.hpp"
#include "manual_clock.hpp"
#include "r2s_bridge/framing.hpp"

extern "C" {
#include "r2s/r2s_crc.h"
#include "r2s/r2s_rcobs.h"
}

namespace r2s_bridge
{
namespace test
{
namespace
{

// The decoded frame size the host accepts, from topic_id to crc16 (FRM-10).
constexpr std::size_t kHostMaxFrame = 65277;

std::vector<std::uint8_t> bytes_of(const std::string &hex)
{
    std::vector<std::uint8_t> bytes;
    std::istringstream in(hex);
    std::string pair;
    while (in >> pair) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16)));
    }
    return bytes;
}

// The encoded form of a decoded_size-byte frame (topic_id 0x10, seq 0, the
// rest fill but for the CRC), built with the core's encoder and check and
// ended by the framer's delimiter. The core's vectors already pin both, so
// this helper only sizes the frames for the limit tests below.
std::vector<std::uint8_t> wire_of(std::size_t decoded_size, std::uint8_t fill = 0xAAu)
{
    std::vector<std::uint8_t> decoded(decoded_size, fill);
    decoded[0u] = 0x10u;
    decoded[1u] = 0x00u;
    const std::uint16_t crc = r2s_crc16_update_buf(R2S_CRC16_INIT, decoded.data(), decoded_size - 2u);
    decoded[decoded_size - 2u] = static_cast<std::uint8_t>(crc & 0xFFu);
    decoded[decoded_size - 1u] = static_cast<std::uint8_t>(crc >> 8u);

    std::vector<std::uint8_t> wire;
    r2s_rcobs_encoder_t encoder{};
    r2s_rcobs_encoder_init(&encoder);
    std::uint8_t chunk[R2S_RCOBS_MAX_EMIT]{};
    for (const std::uint8_t byte : decoded) {
        const std::uint8_t produced = r2s_rcobs_encode_byte(&encoder, byte, chunk);
        wire.insert(wire.end(), chunk, chunk + produced);
    }
    wire.insert(wire.end(), chunk, chunk + r2s_rcobs_encode_end(&encoder, chunk));
    wire.push_back(0x00u);
    return wire;
}

std::vector<std::string> frame_vectors()
{
    std::vector<std::string> files;
    for (const auto &entry : std::filesystem::directory_iterator(R2S_FRAMES_VECTOR_DIR)) {
        if (entry.path().extension() == ".yaml") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

class FrameVector : public ::testing::TestWithParam<std::string>
{
};

TEST_P(FrameVector, DecodesTheWireFormToTheVectorFrame)
{
    const YAML::Node vector = YAML::LoadFile(GetParam());
    const std::vector<std::uint8_t> wire = bytes_of(vector["wire"].as<std::string>());
    const std::vector<std::uint8_t> frame = bytes_of(vector["frame"].as<std::string>());
    ASSERT_FALSE(wire.empty()) << "every vector ends with the 0x00 delimiter";
    ASSERT_EQ(wire.back(), 0x00u);

    FakeTransport transport;
    ManualClock clock;
    FrameReader reader(transport, clock);
    transport.script_read(wire);

    Frame received;
    ASSERT_TRUE(reader.poll(received));
    EXPECT_EQ(std::vector<std::uint8_t>(received.data, received.data + received.size), frame);
    EXPECT_EQ(received.tick, 0u);
    reader.release();
    EXPECT_FALSE(reader.poll(received)) << "the vector holds one frame";
}

INSTANTIATE_TEST_SUITE_P(Frames, FrameVector, ::testing::ValuesIn(frame_vectors()),
                         [](const ::testing::TestParamInfo<std::string> &info) {
                             return std::filesystem::path(info.param).stem().string();
                         });

TEST(FrameReader, ReadsTheTickWhenTheDelimiterArrives)
{
    const std::vector<std::uint8_t> wire = wire_of(6u);
    ASSERT_GT(wire.size(), 2u);

    FakeTransport transport;
    ManualClock clock(std::chrono::milliseconds(40));
    FrameReader reader(transport, clock);
    transport.script_read(std::vector<std::uint8_t>(wire.begin(), wire.end() - 1));
    Frame received;
    EXPECT_FALSE(reader.poll(received));
    clock.advance(std::chrono::microseconds(1500));
    transport.script_read({wire.back()});

    ASSERT_TRUE(reader.poll(received));
    EXPECT_EQ(received.tick, 41u);
}

TEST(FrameReader, AcceptsADecodedFrameAtTheHostMaximum)
{
    const std::vector<std::uint8_t> wire = wire_of(kHostMaxFrame);
    EXPECT_LE(wire.size(), 65535u);

    FakeTransport transport;
    ManualClock clock;
    FrameReader reader(transport, clock);
    transport.script_read(wire);

    Frame received;
    ASSERT_TRUE(reader.poll(received));
    EXPECT_EQ(received.size, kHostMaxFrame);
    EXPECT_EQ(received.data[0u], 0x10u);
    EXPECT_EQ(received.data[1u], 0x00u);
    EXPECT_EQ(r2s_crc16_update_buf(R2S_CRC16_INIT, received.data, received.size), 0u);
}

TEST(FrameReader, DropsADecodedFrameOneByteOverTheHostMaximum)
{
    // Zero fill keeps the encoding inside the buffer bound, so the decoded
    // limit is the rule that drops this frame, not the overflow below it.
    const std::vector<std::uint8_t> wire = wire_of(kHostMaxFrame + 1u, 0x00u);
    EXPECT_LE(wire.size(), 65535u);

    FakeTransport transport;
    ManualClock clock;
    FrameReader reader(transport, clock);
    transport.script_read(wire);

    Frame received;
    EXPECT_FALSE(reader.poll(received));
}

TEST(FrameReader, RefusesASecondReaderInOneProcess)
{
    FakeTransport transport;
    ManualClock clock;
    FrameReader first(transport, clock);

    EXPECT_THROW(FrameReader(transport, clock), std::logic_error);
}

} // namespace
} // namespace test
} // namespace r2s_bridge
