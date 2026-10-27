// The host session: the section 8.2 state machine. Timers advance only in
// poll(), the link is read through the core framer, and every frame the host
// sends is assembled here from the core's CRC and rCOBS.
#include "r2s_bridge/session.hpp"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core_port.hpp"
#include "r2s_bridge/framing.hpp"

extern "C" {
#include "r2s/r2s.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_rcobs.h"
}

namespace r2s_bridge
{
namespace
{

// The control channel's opcodes ([CTL-16]).
constexpr std::uint8_t kHelloReq = 0x01u;
constexpr std::uint8_t kHello = 0x02u;
constexpr std::uint8_t kTopicReq = 0x03u;
constexpr std::uint8_t kTopic = 0x04u;
constexpr std::uint8_t kTopicStatus = 0x05u;
constexpr std::uint8_t kReady = 0x06u;

constexpr std::uint8_t kControlTopic = 0u;
constexpr std::uint8_t kHeartbeatTopic = 4u;

// The spec's cadence ([SES-13] to [SES-18]).
constexpr std::int64_t kHelloReqPeriodMs = 500;
constexpr std::int64_t kTopicTimeoutMs = 200;
constexpr int kTopicRetries = 3;

// The handshake mode byte ([CTL-1]).
constexpr std::uint8_t kModeFull = 0u;

// Bits 6 and 7 are the reserved features and MUST be 0; bit 4 says the MCU's
// `double` is 32 bits and bit 5 runtime registration, both informational
// ([CTL-2], [CTL-3]).
constexpr std::uint8_t kFeaturesReserved = 0xC0u;

// The protocol's own frame limit ([FRM-9]).
constexpr std::uint32_t kMaxFrameLimit = 65277u;

// The payload a HELLO carries, after its opcode ([CTL-16]).
struct HelloFacts {
    std::uint8_t version = 0;
    std::uint8_t mode = 0;
    std::uint32_t boot_id = 0;
    std::uint16_t max_frame = 0;
    std::uint16_t rx_capacity = 0;
    std::uint32_t tick_hz = 0;
    std::uint8_t features = 0;
    std::uint8_t topic_count = 0;
    std::string node_name;
};

// Reads little-endian fields out of a payload, failing when the payload ends
// first. A short payload is a malformed frame, never a partial HELLO.
class Reader
{
  public:
    Reader(const std::uint8_t *data, std::uint16_t len) : data_(data), len_(len)
    {
    }

    bool u8(std::uint8_t &out)
    {
        if (left() < 1u) {
            return false;
        }
        out = data_[pos_++];
        return true;
    }

    bool u16(std::uint16_t &out)
    {
        if (left() < 2u) {
            return false;
        }
        out = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1u] << 8u));
        pos_ += 2u;
        return true;
    }

    bool u32(std::uint32_t &out)
    {
        if (left() < 4u) {
            return false;
        }
        out = static_cast<std::uint32_t>(data_[pos_]) | (static_cast<std::uint32_t>(data_[pos_ + 1u]) << 8u) |
              (static_cast<std::uint32_t>(data_[pos_ + 2u]) << 16u) |
              (static_cast<std::uint32_t>(data_[pos_ + 3u]) << 24u);
        pos_ += 4u;
        return true;
    }

    // A varint length and its bytes, as [ENC-7] lays a string out.
    bool string(std::string &out)
    {
        std::uint32_t length = 0;
        std::uint32_t shift = 0;
        for (;;) {
            std::uint8_t byte = 0;
            if (!u8(byte)) {
                return false;
            }
            length |= static_cast<std::uint32_t>(byte & 0x7Fu) << shift;
            if ((byte & 0x80u) == 0u) {
                // A zero group above the first means the value had a shorter
                // form, which [VAR-4] refuses.
                if (shift != 0u && byte == 0u) {
                    return false;
                }
                break;
            }
            shift += 7u;
            if (shift > 14u) {
                return false;
            }
        }
        if (length > left()) {
            return false;
        }
        out.assign(reinterpret_cast<const char *>(data_ + pos_), length);
        pos_ += static_cast<std::uint16_t>(length);
        return true;
    }

    bool at_end() const
    {
        return pos_ == len_;
    }

  private:
    std::uint16_t left() const
    {
        return static_cast<std::uint16_t>(len_ - pos_);
    }

    const std::uint8_t *data_;
    std::uint16_t len_;
    std::uint16_t pos_ = 0;
};

// tick_hz is 1000 times a divisor of 10^6 ([CTL-8]); anything else is refused
// ([CTL-9]).
bool tick_hz_is_valid(std::uint32_t tick_hz)
{
    if (tick_hz < 1000u || tick_hz % 1000u != 0u) {
        return false;
    }
    const std::uint32_t k = tick_hz / 1000u;
    return 1000000u % k == 0u;
}

} // namespace

struct Session::Impl {
    Impl(SessionConfig in_config, const Clock &in_clock, TransportFactory in_open, TopicResolver in_resolve,
         SessionEvents in_events)
        : config(in_config), clock(in_clock), open(std::move(in_open)), resolve(std::move(in_resolve)),
          events(std::move(in_events))
    {
        if (config.reconnect_period_ms <= 0 || config.boot_wait_ms <= 0 || config.heartbeat_period_ms <= 0 ||
            config.link_timeout_ms <= 0) {
            throw std::invalid_argument("every session period must be positive");
        }
        next_open_ms = now_ms();
        try_open();
    }

    std::int64_t now_ms() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock.now()).count();
    }

    void log(SessionLog level, const std::string &text)
    {
        if (events.log) {
            events.log(level, text);
        }
    }

    void try_open()
    {
        next_open_ms = now_ms() + config.reconnect_period_ms;
        std::unique_ptr<Transport> opened;
        try {
            opened = open();
        } catch (const std::exception &) {
            opened = nullptr;
        }
        if (!opened) {
            return;
        }
        transport = std::move(opened);
        clear_link_fault();
        reader = std::make_unique<FrameReader>(*transport, clock);
        pending.clear();
        state = SessionState::kBootWait;
        boot_wait_deadline_ms = now_ms() + config.boot_wait_ms;
    }

    void close_link()
    {
        reader.reset();
        if (transport) {
            transport->close();
        }
        transport.reset();
        pending.clear();
        awaiting_topic = false;
        state = SessionState::kDisconnected;
        next_open_ms = now_ms() + config.reconnect_period_ms;
    }

    // Queues one frame: the payload through the check, then rCOBS and the
    // delimiter, so the bytes are the ones [FRM-1] and [CHK-3] give.
    void queue_frame(std::uint8_t topic_id, std::uint8_t seq, const std::vector<std::uint8_t> &payload)
    {
        std::vector<std::uint8_t> decoded;
        decoded.reserve(payload.size() + 4u);
        decoded.push_back(topic_id);
        decoded.push_back(seq);
        decoded.insert(decoded.end(), payload.begin(), payload.end());
        const std::uint16_t crc =
            r2s_crc16_update_buf(R2S_CRC16_INIT, decoded.data(), static_cast<std::uint16_t>(decoded.size()));
        decoded.push_back(static_cast<std::uint8_t>(crc & 0xFFu));
        decoded.push_back(static_cast<std::uint8_t>(crc >> 8u));

        r2s_rcobs_encoder_t encoder{};
        r2s_rcobs_encoder_init(&encoder);
        std::uint8_t chunk[R2S_RCOBS_MAX_EMIT]{};
        for (const std::uint8_t byte : decoded) {
            const std::uint8_t produced = r2s_rcobs_encode_byte(&encoder, byte, chunk);
            pending.insert(pending.end(), chunk, chunk + produced);
        }
        const std::uint8_t produced = r2s_rcobs_encode_end(&encoder, chunk);
        pending.insert(pending.end(), chunk, chunk + produced);
        pending.push_back(0x00u);
    }

    void flush()
    {
        if (!transport || pending.empty()) {
            return;
        }
        std::size_t offset = 0;
        while (offset < pending.size()) {
            const IoResult wrote = transport->write(pending.data() + offset, pending.size() - offset);
            if (wrote.status == IoStatus::kWouldBlock) {
                break;
            }
            if (wrote.status != IoStatus::kOk) {
                pending.clear();
                close_link();
                return;
            }
            offset += wrote.bytes;
        }
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    void send_control(const std::vector<std::uint8_t> &payload)
    {
        queue_frame(kControlTopic, seq_control, payload);
        ++seq_control;
    }

    void send_hello_req()
    {
        send_control({kHelloReq, R2S_PROTOCOL_VERSION});
        next_hello_req_ms = now_ms() + kHelloReqPeriodMs;
    }

    void send_topic_req()
    {
        send_control({kTopicReq, static_cast<std::uint8_t>(requested_index)});
        topic_deadline_ms = now_ms() + kTopicTimeoutMs;
    }

    void send_heartbeat()
    {
        queue_frame(kHeartbeatTopic, seq_heartbeat, {});
        ++seq_heartbeat;
        next_heartbeat_ms = now_ms() + config.heartbeat_period_ms;
    }

    void begin_handshake_wait()
    {
        state = SessionState::kHandshaking;
        next_hello_req_ms = now_ms();
    }

    void restart_handshake()
    {
        awaiting_topic = false;
        requested_index = 0;
        retries = 0;
        collected.clear();
        begin_handshake_wait();
    }

    void start_topic_walk()
    {
        requested_index = 0;
        retries = 0;
        collected.clear();
        awaiting_topic = true;
        if (requested_index < hello.topic_count) {
            send_topic_req();
        } else {
            finish_handshake();
        }
    }

    void finish_handshake()
    {
        awaiting_topic = false;
        const bool identical = !had_handshake && collected.empty() ? false : (collected == topics);
        topics = collected;
        had_handshake = true;
        state = SessionState::kConnected;
        send_control({kReady});
        // The handshake is over, so every counter restarts at 0 ([FRM-4]).
        seq_control = 0;
        seq_heartbeat = 0;
        last_rx_ms = now_ms();
        next_heartbeat_ms = now_ms() + config.heartbeat_period_ms;
        if (events.handshake_completed) {
            events.handshake_completed(identical);
        }
    }

    // A HELLO is the MCU announcing itself. Returns whether it was accepted.
    bool on_hello(const std::uint8_t *payload, std::uint16_t len)
    {
        if (len < 1u) {
            return false;
        }
        Reader reader(payload + 1u, static_cast<std::uint16_t>(len - 1u));
        HelloFacts facts;
        if (!reader.u8(facts.version) || !reader.u8(facts.mode) || !reader.u32(facts.boot_id) ||
            !reader.u16(facts.max_frame) || !reader.u16(facts.rx_capacity) || !reader.u32(facts.tick_hz) ||
            !reader.u8(facts.features) || !reader.u8(facts.topic_count)) {
            log(SessionLog::kError, "HELLO refused: the payload is shorter than its fields");
            return false;
        }
        if (facts.version != R2S_PROTOCOL_VERSION) {
            log(SessionLog::kError, "HELLO refused: the MCU speaks protocol version " +
                                        std::to_string(facts.version) + ", this host speaks " +
                                        std::to_string(R2S_PROTOCOL_VERSION));
            return false;
        }
        if (facts.mode != kModeFull) {
            log(SessionLog::kError,
                "HELLO refused: compact mode needs a node map, which is not supported yet");
            return false;
        }
        if ((facts.features & kFeaturesReserved) != 0u) {
            log(SessionLog::kError, "HELLO refused: the features byte has a reserved bit set");
            return false;
        }
        if (!tick_hz_is_valid(facts.tick_hz)) {
            log(SessionLog::kError, "HELLO refused: tick_hz " + std::to_string(facts.tick_hz) +
                                        " is not 1000 times a divisor of 10^6");
            return false;
        }
        if (facts.max_frame > kMaxFrameLimit) {
            log(SessionLog::kError, "HELLO refused: max_frame is above the protocol limit");
            return false;
        }
        if (!reader.string(facts.node_name) || !reader.at_end()) {
            log(SessionLog::kError, "HELLO refused: the node name does not match the payload");
            return false;
        }

        const bool same_boot = seen_boot_id && facts.boot_id == hello.boot_id;
        if (state == SessionState::kHandshaking && awaiting_topic && same_boot) {
            // The MCU repeats HELLO while it hears nothing; the walk under way
            // is the answer to the first one, so a repeat changes nothing.
            return true;
        }
        hello = facts;
        seen_boot_id = true;
        // A HELLO that lands while connected is a reset, and the same HELLO
        // announces the new handshake, so no extra HELLO_REQ is needed
        // ([SES-19], [SES-21]).
        state = SessionState::kHandshaking;
        start_topic_walk();
        return true;
    }

    void on_topic(const std::uint8_t *payload, std::uint16_t len)
    {
        if (!awaiting_topic || len < 1u) {
            return;
        }
        Reader reader(payload + 1u, static_cast<std::uint16_t>(len - 1u));
        TopicInfo info;
        std::uint8_t index = 0;
        if (!reader.u8(index) || !reader.u8(info.topic_id) || !reader.u8(info.flags)) {
            log(SessionLog::kError, "TOPIC refused: the payload is shorter than its fields");
            return;
        }
        if (index != requested_index || info.topic_id < 16u) {
            log(SessionLog::kError, "TOPIC refused: index " + std::to_string(index) + " or topic id " +
                                        std::to_string(info.topic_id) + " is not the one requested");
            return;
        }
        for (std::size_t i = 0; i < info.type_hash.size(); ++i) {
            std::uint8_t byte = 0;
            if (!reader.u8(byte)) {
                log(SessionLog::kError, "TOPIC refused: the type hash does not match the payload");
                return;
            }
            info.type_hash[i] = byte;
        }
        if (!reader.string(info.type_name) || !reader.string(info.topic_name) || !reader.at_end()) {
            log(SessionLog::kError, "TOPIC refused: the names do not match the payload");
            return;
        }
        const std::uint8_t verdict = resolve ? resolve(info) : kTopicStatusOk;
        // The MCU stores the verdict per topic ([SES-2]); the host logs the
        // refusals, since the wire carries only the code.
        if (verdict == kTopicStatusUnknownType) {
            log(SessionLog::kError,
                "TOPIC refused: unknown type '" + info.type_name + "' for topic '" + info.topic_name + "'");
        } else if (verdict == kTopicStatusHashMismatch) {
            log(SessionLog::kError, "TOPIC refused: the hash of '" + info.type_name +
                                        "' is not the installed one, for topic '" + info.topic_name + "'");
        }
        info.status = verdict;
        collected.push_back(info);
        send_control({kTopicStatus, info.topic_id, verdict});
        ++requested_index;
        retries = 0;
        if (requested_index < hello.topic_count) {
            send_topic_req();
        } else {
            finish_handshake();
        }
    }

    void on_control(const std::uint8_t *payload, std::uint16_t len)
    {
        if (len < 1u) {
            return;
        }
        switch (payload[0]) {
        case kHello:
            on_hello(payload, len);
            break;
        case kTopic:
            on_topic(payload, len);
            break;
        default:
            // A control opcode the host does not receive here is ignored;
            // the link's health already counted the frame.
            break;
        }
    }

    void on_frame(const Frame &frame)
    {
        last_rx_ms = now_ms();
        if (frame.size < 4u) {
            return;
        }
        const std::uint8_t topic_id = frame.data[0];
        const std::uint8_t *payload = frame.data + 2u;
        const std::uint16_t len = static_cast<std::uint16_t>(frame.size - 4u);
        if (topic_id == kControlTopic) {
            on_control(payload, len);
            return;
        }
        if (topic_id == kHeartbeatTopic) {
            if (len != 8u) {
                return;
            }
            Reader reader(payload, len);
            McuCounters counters;
            (void)reader.u16(counters.rx_frames);
            (void)reader.u16(counters.crc_errors);
            (void)reader.u16(counters.rx_overflows);
            (void)reader.u16(counters.dropped_frames);
            mcu = counters;
            return;
        }
        // The other system channels and user topics belong to later tickets;
        // the frame still keeps the link alive.
    }

    void run_timers()
    {
        const std::int64_t now = now_ms();
        if (state == SessionState::kDisconnected) {
            if (now >= next_open_ms) {
                try_open();
            }
            return;
        }
        switch (state) {
        case SessionState::kBootWait:
            if (now >= boot_wait_deadline_ms) {
                // The board is either done booting or not talking at all;
                // ask for its HELLO from now on ([SES-13], [SES-14]).
                begin_handshake_wait();
                send_hello_req();
            }
            break;
        case SessionState::kHandshaking:
            if (awaiting_topic) {
                if (now >= topic_deadline_ms) {
                    if (retries >= kTopicRetries) {
                        log(SessionLog::kWarning, "no TOPIC answered; the handshake starts over");
                        restart_handshake();
                        send_hello_req();
                    } else {
                        ++retries;
                        send_topic_req();
                    }
                }
            } else if (now >= next_hello_req_ms) {
                send_hello_req();
            }
            break;
        case SessionState::kConnected:
            if (now - last_rx_ms >= config.link_timeout_ms) {
                log(SessionLog::kWarning, "no frame from the MCU within the link timeout");
                restart_handshake();
                send_hello_req();
            } else if (now >= next_heartbeat_ms) {
                send_heartbeat();
            }
            break;
        default:
            break;
        }
    }

    void poll()
    {
        flush();
        if (reader) {
            Frame frame;
            while (reader && reader->poll(frame)) {
                on_frame(frame);
                reader->release();
                flush();
            }
            if (link_faulted()) {
                close_link();
                return;
            }
        }
        run_timers();
        flush();
    }

    SessionConfig config;
    const Clock &clock;
    TransportFactory open;
    TopicResolver resolve;
    SessionEvents events;

    std::unique_ptr<Transport> transport;
    std::unique_ptr<FrameReader> reader;
    std::vector<std::uint8_t> pending;

    SessionState state = SessionState::kDisconnected;
    std::int64_t next_open_ms = 0;
    std::int64_t boot_wait_deadline_ms = 0;
    std::int64_t next_hello_req_ms = 0;
    std::int64_t topic_deadline_ms = 0;
    std::int64_t next_heartbeat_ms = 0;
    std::int64_t last_rx_ms = 0;

    HelloFacts hello;
    bool seen_boot_id = false;
    bool awaiting_topic = false;
    std::uint8_t requested_index = 0;
    int retries = 0;

    std::vector<TopicInfo> collected;
    std::vector<TopicInfo> topics;
    bool had_handshake = false;
    McuCounters mcu;

    std::uint8_t seq_control = 0;
    std::uint8_t seq_heartbeat = 0;
};

Session::Session(SessionConfig config, const Clock &clock, TransportFactory open, TopicResolver resolve,
                 SessionEvents events)
    : impl_(std::make_unique<Impl>(config, clock, std::move(open), std::move(resolve), std::move(events)))
{
}

Session::~Session() = default;

SessionState Session::state() const
{
    return impl_->state;
}

bool Session::has_boot_id() const
{
    return impl_->seen_boot_id;
}

std::uint32_t Session::boot_id() const
{
    return impl_->hello.boot_id;
}

const std::vector<TopicInfo> &Session::topics() const
{
    return impl_->topics;
}

McuCounters Session::mcu_counters() const
{
    return impl_->mcu;
}

void Session::poll()
{
    impl_->poll();
}

} // namespace r2s_bridge
