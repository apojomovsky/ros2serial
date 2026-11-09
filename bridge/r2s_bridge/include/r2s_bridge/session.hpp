// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// The host side of the session: boot wait, handshake and reset rules
// (spec/protocol.md, section 8.2). A plain C++ class over Transport and Clock,
// so tests drive it with the fake transport and the manual clock.
#ifndef R2S_BRIDGE__SESSION_HPP_
#define R2S_BRIDGE__SESSION_HPP_

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "r2s_bridge/clock.hpp"
#include "r2s_bridge/transport.hpp"

namespace r2s_bridge
{

// The states of the section 8.2 diagram. DISCONNECTED owns opening the port;
// BOOT_WAIT owns the wait for an unsolicited HELLO after the open.
enum class SessionState : std::uint8_t
{
  kDisconnected,
  kBootWait,
  kHandshaking,
  kConnected,
};

// Severity of a SessionEvents::log or ::mcu_log call. The bridge maps these
// to its loggers with the same severity.
enum class SessionLog : std::uint8_t
{
  kError,
  kWarning,
  kInfo,
  kDebug,
  kFatal,
};

// The TOPIC_STATUS verdicts ([CTL-13]): 3 names a topic the bridge cannot
// serve yet, when two announced topics claim one name; H7 detects it.
constexpr std::uint8_t kTopicStatusOk = 0u;
constexpr std::uint8_t kTopicStatusUnknownType = 1u;
constexpr std::uint8_t kTopicStatusHashMismatch = 2u;
constexpr std::uint8_t kTopicStatusNameConflict = 3u;

// One announced topic, as the TOPIC frame carries it. The flags are the wire
// bits ([CTL-4]); H5 checks the hash and H7 routes the traffic.
struct TopicInfo
{
  std::uint8_t topic_id = 0;
  std::uint8_t flags = 0;
  std::array<std::uint8_t, 8> type_hash{};
  std::string type_name;
  std::string topic_name;
    // The host's TOPIC_STATUS verdict for this topic, one of the codes above.
    // H7 forwards only topics with 0. It is the host's answer, not the MCU's
    // announcement, so it never decides whether two tables match.
  std::uint8_t status = kTopicStatusOk;

    // True when the MCU publishes the topic, false when it subscribes.
  bool device_to_host() const
  {
    return (flags & 0x01u) == 0u;
  }
  bool narrow() const
  {
    return (flags & 0x02u) != 0u;
  }
  bool keep_latest() const
  {
    return (flags & 0x04u) != 0u;
  }
  bool reliable() const
  {
    return (flags & 0x08u) != 0u;
  }
  bool operator==(const TopicInfo & other) const
  {
    return topic_id == other.topic_id && flags == other.flags && type_hash == other.type_hash &&
           type_name == other.type_name && topic_name == other.topic_name;
  }
  bool operator!=(const TopicInfo & other) const
  {
    return !(*this == other);
  }
};

// The MCU heartbeat counters ([HB-2]), kept for the diagnostics ticket (H8).
struct McuCounters
{
  std::uint16_t rx_frames = 0;
  std::uint16_t crc_errors = 0;
  std::uint16_t rx_overflows = 0;
  std::uint16_t dropped_frames = 0;
  bool operator==(const McuCounters & other) const
  {
    return rx_frames == other.rx_frames && crc_errors == other.crc_errors &&
           rx_overflows == other.rx_overflows && dropped_frames == other.dropped_frames;
  }
  bool operator!=(const McuCounters & other) const
  {
    return !(*this == other);
  }
};

// Timing of the state machine. The spec fixes the handshake cadence; the
// parameters below default to the bridge parameters of the same name.
struct SessionConfig
{
  int reconnect_period_ms = 1000;
  int boot_wait_ms = 2500;
  int heartbeat_period_ms = 500;
  int link_timeout_ms = 2000;
    // Time sync (spec/protocol.md, section 8.3): PINGs after READY and the
    // estimator behind the CORRECTIONs. A period outside 1 to 5000 ms throws,
    // because SES-25 needs a correction at least every 5 s.
  bool time_sync_enabled = true;
  int time_sync_period_ms = 5000;
};

// Opens the port. Returns the transport, or null when the port is not there
// yet; the session tries again every reconnect_period_ms. May throw, which
// counts as a failed open.
using TransportFactory = std::function<std::unique_ptr<Transport>()>;

// Answers one announced topic with its TOPIC_STATUS verdict, one of the
// codes above. type_support.hpp loads the verdict from the installed type
// support; tests script it.
using TopicResolver = std::function<std::uint8_t(const TopicInfo &)>;

// One announced MCU parameter: the PAR-2 type, the PAR-4 flags, the name,
// and the default value as its wire bytes (PAR-1, PAR-3).
struct ParamInfo
{
  std::uint8_t index = 0;
  std::uint8_t type = 0;
  std::uint8_t flags = 0;
  std::string name;
  std::vector<std::uint8_t> default_value;
  bool operator==(const ParamInfo & other) const
  {
    return index == other.index && type == other.type && flags == other.flags &&
           name == other.name &&
           default_value == other.default_value;
  }
  bool operator!=(const ParamInfo & other) const
  {
    return !(*this == other);
  }
};

// The session's observable edge: log lines the bridge forwards to its
// loggers, one call per completed handshake saying whether the new topic
// table matches the previous one ([SES-20]), and one call per decoded user
// payload from a topic whose handshake verdict is ok. The MCU's own log
// lines ([LOG-1]) arrive on mcu_log with the severity [LOG-2] names, so the
// bridge can log them under its own child name; all default to ignoring.
struct SessionEvents
{
  std::function<void(SessionLog, const std::string &)> log;
  std::function<void(bool identical)> handshake_completed;
  std::function<void(std::uint8_t, const std::vector<std::uint8_t> &)> user_frame;
  std::function<void(SessionLog, const std::string &)> mcu_log;
    // One PARAM_STATUS the MCU reported while connected, as index and status
    // code. The bridge answers every reliable frame itself; this reports the
    // verdicts the parameters layer (X5) consumes.
  std::function<void(std::uint8_t, std::uint8_t)> param_status;
};

// The section 8.2 state machine. poll() advances timers, drains the link and
// sends what is due; nothing else moves time or touches the port. One session
// per process: it owns the core framer's single port binding while open.
class Session
{
public:
    // Throws std::invalid_argument when a period is not positive, or when the
    // time sync period is outside 1 to 5000 ms. The ROS clock stamps the
    // CORRECTIONs; the steady clock measures the round trips and the timers.
  Session(
    SessionConfig config, const Clock & clock, const Clock & ros_clock, TransportFactory open,
    TopicResolver resolve, SessionEvents events = SessionEvents());
  ~Session();
  Session(const Session &) = delete;
  Session & operator=(const Session &) = delete;

  SessionState state() const;
    // Whether a HELLO has been accepted. The recorded token below is the one
    // READY assigned after a completed handshake, and the MCU's echo
    // otherwise: 0 before any READY and after every reboot ([SES-19]).
  bool seen_hello() const;
  std::uint32_t token() const;
    // The last completed handshake's topics, empty before the first one.
  const std::vector<TopicInfo> & topics() const;
    // The last completed handshake's parameters, in announcement order,
    // empty before the first one. Every entry parsed cleanly: the walk
    // keeps no short payload, unknown type, reserved flag, overrun name or
    // misshapen default, so the parameters layer lays the bytes out
    // without rechecking them.
  const std::vector<ParamInfo> & params() const;
  McuCounters mcu_counters() const;
    // Running sums of the MCU error report counts per code ([ERR-6]): each
    // decoded report adds its count to its code's slot, so a wrapped count
    // still adds up. Slot 0 carries code 1 through slot 5 code 6, counting
    // targeted and untargeted reports alike. Zero before the first report;
    // a bridge restart resets every sum.
  std::array<std::uint64_t, 6> error_sums() const;
    // Lost MCU-to-host frames per user topic ([FRM-15]): the sum of
    // (seq - expected) mod 256 over every user frame received while
    // connected. The first frame after each handshake expects seq 0, so a
    // first seq of N counts N; gaps accumulate across handshakes.
  std::uint32_t seq_gaps(std::uint8_t topic_id) const;
    // MCU error report counts naming one topic id ([ERR-6]), in the same
    // slots as error_sums. Zeros when no report named the id, including
    // ids the handshake never announced and the 0xFF no-topic sentinel.
  std::array<std::uint64_t, 6> error_counts(std::uint8_t topic_id) const;
    // HELLOs accepted while connected: the MCU rebooted and announced a new
    // handshake ([SES-19], [SES-21]). Link timeouts re-handshake without a
    // HELLO and do not count.
  std::uint32_t resets() const;
    // Whether the estimator has anchored a correction: false before the first
    // CORRECTION, after every re-handshake, and while time sync is disabled.
  bool time_available() const;
    // The H8 time sync values: the fitted ROS time at MCU tick zero, the
    // fitted drift, the latest round trip. Zero while unavailable.
  std::int64_t time_offset_ns() const;
  std::int16_t time_drift_ppm() const;
  std::int64_t time_round_trip_ns() const;
    // Unsent bytes held for the link, user and control frames together.
  std::size_t pending_bytes() const;
    // Control frames a newer frame of the same slot replaced while the link
    // could not take bytes, for the H8 diagnostics ([SES-33]).
  std::uint32_t control_coalesced() const;
    // Queues one host-to-MCU payload on its topic, assigning the topic's next
    // seq and flushing the link. The seq restarts at 0 after each handshake.
    // False when the link already holds the MCU's receive capacity in unsent
    // bytes: the frame waits for the link instead of growing the host heap
    // ([SES-32]). True always goes out when nothing is pending.
  bool send_user(std::uint8_t topic_id, const std::vector<std::uint8_t> & payload);
    // Queues one PARAM_SET on the reliable channel (REL-2, REL-7): the value
    // bytes travel after the index, laid out the way section 7.6 lays the
    // value out. False unless connected; a queued SET goes out behind the
    // outstanding one, at most one per index, with the next rel seq.
  bool send_param_set(std::uint8_t index, const std::vector<std::uint8_t> & value);
    // PARAM_SET frames the spent retry dropped ([REL-2]), for diagnostics.
  std::uint32_t reliable_dropped() const;
    // Queued SETs a newer SET to the same index replaced ([REL-7]), for
    // diagnostics.
  std::uint32_t reliable_replaced() const;
    // The MCU's announced receive limits, 0 before the first HELLO. A sender
    // stays within both ([FRM-11]).
  std::uint16_t mcu_max_frame() const;
  std::uint16_t mcu_rx_capacity() const;
  void poll();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__SESSION_HPP_
