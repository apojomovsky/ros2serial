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
enum class SessionState : std::uint8_t {
    kDisconnected,
    kBootWait,
    kHandshaking,
    kConnected,
};

// Severity of a SessionEvents::log call. The bridge maps these to its loggers.
enum class SessionLog : std::uint8_t {
    kError,
    kWarning,
    kInfo,
};

// The TOPIC_STATUS verdicts ([CTL-13]): 3 names a topic the bridge cannot
// serve yet, when two announced topics claim one name; H7 detects it.
constexpr std::uint8_t kTopicStatusOk = 0u;
constexpr std::uint8_t kTopicStatusUnknownType = 1u;
constexpr std::uint8_t kTopicStatusHashMismatch = 2u;
constexpr std::uint8_t kTopicStatusNameConflict = 3u;

// One announced topic, as the TOPIC frame carries it. The flags are the wire
// bits ([CTL-4]); H5 checks the hash and H7 routes the traffic.
struct TopicInfo {
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
    bool operator==(const TopicInfo &other) const
    {
        return topic_id == other.topic_id && flags == other.flags && type_hash == other.type_hash &&
               type_name == other.type_name && topic_name == other.topic_name;
    }
    bool operator!=(const TopicInfo &other) const
    {
        return !(*this == other);
    }
};

// The MCU heartbeat counters ([HB-2]), kept for the diagnostics ticket (H8).
struct McuCounters {
    std::uint16_t rx_frames = 0;
    std::uint16_t crc_errors = 0;
    std::uint16_t rx_overflows = 0;
    std::uint16_t dropped_frames = 0;
    bool operator==(const McuCounters &other) const
    {
        return rx_frames == other.rx_frames && crc_errors == other.crc_errors &&
               rx_overflows == other.rx_overflows && dropped_frames == other.dropped_frames;
    }
    bool operator!=(const McuCounters &other) const
    {
        return !(*this == other);
    }
};

// Timing of the state machine. The spec fixes the handshake cadence; the
// parameters below default to the bridge parameters of the same name.
struct SessionConfig {
    int reconnect_period_ms = 1000;
    int boot_wait_ms = 2500;
    int heartbeat_period_ms = 500;
    int link_timeout_ms = 2000;
};

// Opens the port. Returns the transport, or null when the port is not there
// yet; the session tries again every reconnect_period_ms. May throw, which
// counts as a failed open.
using TransportFactory = std::function<std::unique_ptr<Transport>()>;

// Answers one announced topic with its TOPIC_STATUS verdict, one of the
// codes above. type_support.hpp loads the verdict from the installed type
// support; tests script it.
using TopicResolver = std::function<std::uint8_t(const TopicInfo &)>;

// The session's observable edge: log lines the bridge forwards to its
// loggers, and one call per completed handshake saying whether the new topic
// table matches the previous one ([SES-20]). Both default to ignoring.
struct SessionEvents {
    std::function<void(SessionLog, const std::string &)> log;
    std::function<void(bool identical)> handshake_completed;
};

// The section 8.2 state machine. poll() advances timers, drains the link and
// sends what is due; nothing else moves time or touches the port. One session
// per process: it owns the core framer's single port binding while open.
class Session
{
  public:
    // Throws std::invalid_argument when a period is not positive.
    Session(SessionConfig config, const Clock &clock, TransportFactory open, TopicResolver resolve,
            SessionEvents events = SessionEvents());
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    SessionState state() const;
    // Whether a HELLO has been accepted. The recorded token below is the one
    // READY assigned after a completed handshake, and the MCU's echo
    // otherwise: 0 before any READY and after every reboot ([SES-19]).
    bool seen_hello() const;
    std::uint32_t token() const;
    // The last completed handshake's topics, empty before the first one.
    const std::vector<TopicInfo> &topics() const;
    McuCounters mcu_counters() const;
    void poll();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__SESSION_HPP_
