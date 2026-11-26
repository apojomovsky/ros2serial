// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// Drives one bridge session over an input of decoded-frame records, for
// the fuzz target and the seed probe: a topic id, a seq, a little-endian
// payload length and the payload per record. The check and the rCOBS go
// over each record, it is scripted through the fake transport, and the
// session is polled on the manual clock.
#ifndef BRIDGE__R2S_BRIDGE__FUZZ__BRIDGE_SESSION_HARNESS_HPP_
#define BRIDGE__R2S_BRIDGE__FUZZ__BRIDGE_SESSION_HARNESS_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "fake_transport.hpp"
#include "frame_helpers.hpp"
#include "manual_clock.hpp"
#include "r2s_bridge/session.hpp"

namespace r2s_bridge
{
namespace fuzz
{

/// Whether one input of decoded-frame records reaches the connected state.
/**
 * At most 64 records are read and the clocks advance 10 ms between polls,
 * so timers, retries and the link timeout fire inside one input. The
 * connected state may be left again before the last poll, so it is
 * reported from any poll, not from the last one.
 *
 * \param[in] data the records to feed.
 * \param[in] size how many bytes data holds.
 * \return True when any poll leaves the session connected.
 */
inline bool drive_bridge_session(const std::uint8_t * data, std::size_t size)
{
  test::ManualClock clock;
  test::ManualClock ros_clock;
  test::FakeTransport transport;

  std::size_t offset = 0;
  std::size_t records = 0;
  while (offset + 4u <= size && records < 64u) {
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
    transport.script_read(test::helper_wire_of(test::helper_with_check(decoded)));
  }
  if (records == 0u) {
    return false;
  }

  SessionEvents events;
  events.user_frame = [](std::uint8_t, const std::vector<std::uint8_t> &) {};
  Session session(
    SessionConfig{}, clock, ros_clock,
    [&transport]() {
      return std::unique_ptr<Transport>(new test::HelperLink(transport));
    },
    [](const TopicInfo &) {return std::uint8_t{0};}, events,
    [](const ServiceInfo &) {return std::uint8_t{0};},
    [](const ActionInfo &) {return std::uint8_t{0};});
  bool connected = false;
  session.poll();
  connected = connected || session.state() == SessionState::kConnected;
  for (int round = 0; round < 256; ++round) {
    clock.advance(std::chrono::milliseconds(10));
    ros_clock.advance(std::chrono::milliseconds(10));
    session.poll();
    connected = connected || session.state() == SessionState::kConnected;
  }
  return connected;
}

}  // namespace fuzz
}  // namespace r2s_bridge

#endif  // BRIDGE__R2S_BRIDGE__FUZZ__BRIDGE_SESSION_HARNESS_HPP_
