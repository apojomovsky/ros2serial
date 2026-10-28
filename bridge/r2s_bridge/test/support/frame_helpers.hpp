// Handshake and frame builders shared by the bridge tests that script the
// link: the wire form of decoded frames, HELLO and TOPIC payloads, and a
// transport that borrows the fake instead of owning the link.
#ifndef R2S_BRIDGE__TEST_FRAME_HELPERS_HPP_
#define R2S_BRIDGE__TEST_FRAME_HELPERS_HPP_

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "fake_transport.hpp"
#include "r2s_bridge/transport.hpp"
extern "C" {
#include "r2s/r2s_crc.h"
#include "r2s/r2s_rcobs.h"
}

namespace r2s_bridge
{
namespace test
{

constexpr std::uint8_t kHelperControl = 0u;

inline std::vector<std::uint8_t> helper_bytes_of(const std::string &hex)
{
    std::vector<std::uint8_t> bytes;
    std::istringstream in(hex);
    std::string pair;
    while (in >> pair) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16)));
    }
    return bytes;
}

// The wire form of a decoded frame: rCOBS and the delimiter.
inline std::vector<std::uint8_t> helper_wire_of(const std::vector<std::uint8_t> &decoded)
{
    std::vector<std::uint8_t> wire;
    r2s_rcobs_encoder_t encoder{};
    r2s_rcobs_encoder_init(&encoder);
    std::uint8_t chunk[R2S_RCOBS_MAX_EMIT]{};
    for (const std::uint8_t byte : decoded) {
        const std::uint8_t produced = r2s_rcobs_encode_byte(&encoder, byte, chunk);
        wire.insert(wire.end(), chunk, chunk + produced);
    }
    const std::uint8_t produced = r2s_rcobs_encode_end(&encoder, chunk);
    wire.insert(wire.end(), chunk, chunk + produced);
    wire.push_back(0x00u);
    return wire;
}

// A decoded frame without its check, now with it.
inline std::vector<std::uint8_t> helper_with_check(std::vector<std::uint8_t> frame)
{
    const std::uint16_t crc =
        r2s_crc16_update_buf(R2S_CRC16_INIT, frame.data(), static_cast<std::uint16_t>(frame.size()));
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFu));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8u));
    return frame;
}

// One wire unit per delimiter, dropping the empty ones a flush MAY send.
inline std::vector<std::vector<std::uint8_t>> helper_units_of(const std::vector<std::uint8_t> &bytes)
{
    std::vector<std::vector<std::uint8_t>> units;
    std::vector<std::uint8_t> current;
    for (const std::uint8_t byte : bytes) {
        current.push_back(byte);
        if (byte == 0x00u) {
            if (current.size() > 1u) {
                units.push_back(current);
            }
            current.clear();
        }
    }
    return units;
}

// One decoded wire unit back to its frame, through the stateless decoder.
inline std::vector<std::uint8_t> helper_decode_unit(std::vector<std::uint8_t> unit)
{
    unit.pop_back();
    std::uint8_t *decoded = nullptr;
    std::uint16_t decoded_len = 0;
    if (r2s_rcobs_decode(unit.data(), static_cast<std::uint16_t>(unit.size()), &decoded, &decoded_len) !=
        R2S_RCOBS_OK) {
        return {};
    }
    return {decoded, decoded + decoded_len};
}

// One HELLO payload: the fields of [CTL-16]. The token is the MCU's echo:
// 0 before the host assigns one.
inline std::vector<std::uint8_t> helper_hello(std::uint32_t token, std::uint8_t topics,
                                              std::uint16_t max_frame = 512u)
{
    std::vector<std::uint8_t> p{0x02u, 0x02u, 0x00u};
    const auto put_le = [&p](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            p.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
        }
    };
    put_le(token, 4);
    put_le(max_frame, 2);
    put_le(515, 2);
    put_le(1000000u, 4);
    p.push_back(0x00u);
    p.push_back(topics);
    const std::string name = "node";
    p.push_back(static_cast<std::uint8_t>(name.size()));
    p.insert(p.end(), name.begin(), name.end());
    return p;
}

// One TOPIC payload, with the caller's flags, hash, type and name.
inline std::vector<std::uint8_t> helper_topic(std::uint8_t index, std::uint8_t topic_id, std::uint8_t flags,
                                              const std::uint8_t *hash, const std::string &type,
                                              const std::string &name)
{
    std::vector<std::uint8_t> p{0x04u, index, topic_id, flags};
    p.insert(p.end(), hash, hash + 8u);
    p.push_back(static_cast<std::uint8_t>(type.size()));
    p.insert(p.end(), type.begin(), type.end());
    p.push_back(static_cast<std::uint8_t>(name.size()));
    p.insert(p.end(), name.begin(), name.end());
    return p;
}

// The session opens the test's own fake, so a reopen keeps its script and
// capture. It never closes that fake.
class HelperLink final : public Transport
{
  public:
    explicit HelperLink(FakeTransport &inner) : inner_(inner)
    {
    }
    IoResult read(std::uint8_t *buffer, std::size_t capacity) override
    {
        return inner_.read(buffer, capacity);
    }
    IoResult write(const std::uint8_t *data, std::size_t size) override
    {
        return inner_.write(data, size);
    }
    void close() override
    {
    }

  private:
    FakeTransport &inner_;
};

} // namespace test
} // namespace r2s_bridge

#endif // R2S_BRIDGE__TEST_FRAME_HELPERS_HPP_
