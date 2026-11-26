// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// Fuzz target: the bridge session on arbitrary decoded frames. Each input
// record is a topic id, a seq, a little-endian payload length and the
// payload; the harness lays the check and the rCOBS over it, scripts it
// through the fake transport and polls the session on the manual clock.
// The session must take any frame without crashing, hanging or touching
// memory out of bounds; the sanitizers are the oracle. The transcript
// seeds open with a valid HELLO, so a seeded run gets past the handshake
// into the TOPIC, PARAM, SVC and ACT walks and connected traffic.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "fake_transport.hpp"
#include "frame_helpers.hpp"
#include "manual_clock.hpp"
#include "r2s_bridge/session.hpp"

namespace
{

constexpr std::size_t kMaxRecords = 64u;
constexpr int kPollRounds = 256;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t * data, std::size_t size)
{
  r2s_bridge::test::ManualClock clock;
  r2s_bridge::test::ManualClock ros_clock;
  r2s_bridge::test::FakeTransport transport;

  std::size_t offset = 0;
  std::size_t records = 0;
  while (offset + 4u <= size && records < kMaxRecords) {
    const std::uint8_t topic_id = data[offset];
    const std::uint8_t seq = data[offset + 1u];
    const std::size_t length = static_cast<std::size_t>(data[offset + 2u]) |
      (static_cast<std::size_t>(data[offset + 3u]) << 8u);
    offset += 4u;
    if (length > size - offset) {
      break;
    }
    std::vector<std::uint8_t> decoded{topic_id, seq};
    decoded.insert(decoded.end(), data + offset, data + offset + length);
    offset += length;
    ++records;
    transport.script_read(
      r2s_bridge::test::helper_wire_of(r2s_bridge::test::helper_with_check(decoded)));
  }
  if (records == 0u) {
    return 0;
  }

  r2s_bridge::SessionEvents events;
  events.user_frame = [](std::uint8_t, const std::vector<std::uint8_t> &) {};
  r2s_bridge::Session session(
    r2s_bridge::SessionConfig{}, clock, ros_clock,
    [&transport]() {
      return std::unique_ptr<r2s_bridge::Transport>(
        new r2s_bridge::test::HelperLink(transport));
    },
    [](const r2s_bridge::TopicInfo &) {return std::uint8_t{0};}, events,
    [](const r2s_bridge::ServiceInfo &) {return std::uint8_t{0};},
    [](const r2s_bridge::ActionInfo &) {return std::uint8_t{0};});
  session.poll();
  for (int round = 0; round < kPollRounds; ++round) {
    clock.advance(std::chrono::milliseconds(10));
    ros_clock.advance(std::chrono::milliseconds(10));
    session.poll();
  }
  return 0;
}
