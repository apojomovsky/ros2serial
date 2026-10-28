// The host session: the section 8.2 state machine. Timers advance only in
// poll(), the link is read through the core framer, and every frame the host
// sends is assembled here from the core's CRC and rCOBS.
#include "r2s_bridge/session.hpp"

#include <algorithm>
#include <cstddef>
#include <deque>
#include <map>
#include <stdexcept>
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

// One frame waiting for the link, encoded exactly as [FRM-1] and [CHK-3] lay
// it out. User frames never coalesce ([SES-32]); a control frame carries the
// slot it competes for while the link cannot take bytes ([SES-33]).
struct QueuedFrame {
    std::vector<std::uint8_t> bytes;
    bool control = false;
    std::uint8_t topic_id = 0;
    std::uint8_t opcode = 0;
    std::uint8_t sub = 0;
    std::uint8_t seq = 0;
};

// The slot a control payload competes for: one per control kind, and one
// TOPIC_STATUS per topic id, the field the verdict carries on the wire. The
// time channel has no sender yet, but the same shape covers its PING retries
// when the time sync lands.
QueuedFrame control_slot(std::uint8_t topic_id, const std::vector<std::uint8_t> &payload)
{
    QueuedFrame slot;
    slot.control = true;
    slot.topic_id = topic_id;
    if (!payload.empty()) {
        slot.opcode = payload[0];
        if (topic_id == kControlTopic && payload[0] == kTopicStatus && payload.size() > 1u) {
            slot.sub = payload[1];
        }
    }
    return slot;
}

// The protocol's own frame limit ([FRM-9]).
constexpr std::uint32_t kMaxFrameLimit = 65277u;

// The payload a HELLO carries, after its opcode ([CTL-16]).
struct HelloFacts {
    std::uint8_t version = 0;
    std::uint8_t mode = 0;
    std::uint32_t token = 0;
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
        clear_queue();
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
        clear_queue();
        awaiting_topic = false;
        state = SessionState::kDisconnected;
        next_open_ms = now_ms() + config.reconnect_period_ms;
    }

    void clear_queue()
    {
        queue_.clear();
        pending_bytes_ = 0;
        front_offset_ = 0;
    }

    // Drops the fully unsent control frames. The MCU never saw them, so the
    // new handshake answers instead of the old session's retries ([SES-33]).
    void clear_pending_control()
    {
        std::deque<QueuedFrame> kept;
        std::size_t kept_bytes = 0;
        for (std::size_t i = 0; i < queue_.size(); ++i) {
            if (i > 0 || front_offset_ == 0) {
                if (queue_[i].control) {
                    continue;
                }
            }
            kept_bytes += queue_[i].bytes.size();
            kept.push_back(std::move(queue_[i]));
        }
        queue_ = std::move(kept);
        pending_bytes_ = kept_bytes;
    }

    // Queues one frame: the payload through the check, then rCOBS and the
    // delimiter, so the bytes are the ones [FRM-1] and [CHK-3] give. A
    // control frame whose slot is already queued replaces it in place, so
    // the wire order never changes and the queue stays bounded ([SES-33]).
    // The replaced frame never reached the wire, so the new one keeps its
    // seq and [FRM-5] continuity holds.
    void queue_frame(std::uint8_t topic_id, std::uint8_t seq, const std::vector<std::uint8_t> &payload)
    {
        QueuedFrame frame;
        std::size_t match = queue_.size();
        if (topic_id < 16u) {
            frame = control_slot(topic_id, payload);
            const std::size_t first = front_offset_ > 0u ? 1u : 0u;
            for (std::size_t i = first; i < queue_.size(); ++i) {
                const QueuedFrame &slot = queue_[i];
                if (slot.control && slot.topic_id == frame.topic_id && slot.opcode == frame.opcode &&
                    slot.sub == frame.sub) {
                    match = i;
                    seq = slot.seq;
                    break;
                }
            }
        }
        std::vector<std::uint8_t> decoded;
        decoded.reserve(payload.size() + 4u);
        decoded.push_back(topic_id);
        decoded.push_back(seq);
        decoded.insert(decoded.end(), payload.begin(), payload.end());
        const std::uint16_t crc =
            r2s_crc16_update_buf(R2S_CRC16_INIT, decoded.data(), static_cast<std::uint16_t>(decoded.size()));
        decoded.push_back(static_cast<std::uint8_t>(crc & 0xFFu));
        decoded.push_back(static_cast<std::uint8_t>(crc >> 8u));

        frame.seq = seq;
        r2s_rcobs_encoder_t encoder{};
        r2s_rcobs_encoder_init(&encoder);
        std::uint8_t chunk[R2S_RCOBS_MAX_EMIT]{};
        for (const std::uint8_t byte : decoded) {
            const std::uint8_t produced = r2s_rcobs_encode_byte(&encoder, byte, chunk);
            frame.bytes.insert(frame.bytes.end(), chunk, chunk + produced);
        }
        const std::uint8_t produced = r2s_rcobs_encode_end(&encoder, chunk);
        frame.bytes.insert(frame.bytes.end(), chunk, chunk + produced);
        frame.bytes.push_back(0x00u);
        if (match < queue_.size()) {
            QueuedFrame &slot = queue_[match];
            pending_bytes_ -= slot.bytes.size();
            pending_bytes_ += frame.bytes.size();
            slot.bytes = std::move(frame.bytes);
            ++control_coalesced_;
            return;
        }
        pending_bytes_ += frame.bytes.size();
        queue_.push_back(std::move(frame));
    }

    void flush()
    {
        if (!transport) {
            return;
        }
        while (!queue_.empty()) {
            QueuedFrame &front = queue_.front();
            while (front_offset_ < front.bytes.size()) {
                const IoResult wrote =
                    transport->write(front.bytes.data() + front_offset_, front.bytes.size() - front_offset_);
                if (wrote.status == IoStatus::kWouldBlock) {
                    return;
                }
                if (wrote.status != IoStatus::kOk) {
                    clear_queue();
                    close_link();
                    return;
                }
                front_offset_ += wrote.bytes;
            }
            pending_bytes_ -= front.bytes.size();
            queue_.pop_front();
            front_offset_ = 0;
        }
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
        clear_pending_control();
        begin_handshake_wait();
    }

    void start_topic_walk()
    {
        requested_index = 0;
        retries = 0;
        collected.clear();
        awaiting_topic = true;
        clear_pending_control();
        if (requested_index < hello.topic_count) {
            send_topic_req();
        } else {
            finish_handshake();
        }
    }

    // A fresh nonzero token for the handshake: 0 means no token yet, so the
    // counter wraps from 2^32 - 1 to 1 without a branch ([SES-10], [SES-17]).
    std::uint32_t assign_token()
    {
        next_token = next_token % 0xFFFFFFFFu + 1u;
        return next_token;
    }

    void finish_handshake()
    {
        awaiting_topic = false;
        // The first handshake has no previous table, so nothing can match it.
        const bool identical = had_handshake && collected == topics;
        topics = collected;
        had_handshake = true;
        state = SessionState::kConnected;
        hello.token = assign_token();
        send_control({kReady, static_cast<std::uint8_t>(hello.token & 0xFFu),
                      static_cast<std::uint8_t>((hello.token >> 8u) & 0xFFu),
                      static_cast<std::uint8_t>((hello.token >> 16u) & 0xFFu),
                      static_cast<std::uint8_t>((hello.token >> 24u) & 0xFFu)});
        // The handshake is over, so every counter restarts at 0 ([FRM-4]).
        seq_control = 0;
        seq_heartbeat = 0;
        user_seq_.clear();
        expected_seq.clear();
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
        if (!reader.u8(facts.version) || !reader.u8(facts.mode) || !reader.u32(facts.token) ||
            !reader.u16(facts.max_frame) || !reader.u16(facts.rx_capacity) || !reader.u32(facts.tick_hz) ||
            !reader.u8(facts.features) || !reader.u8(facts.topic_count)) {
            log(SessionLog::kError, "HELLO refused: the payload is shorter than its fields");
            return false;
        }
        if (facts.version != R2S_PROTOCOL_VERSION) {
            log(SessionLog::kError, "HELLO refused: the MCU protocol version is " +
                                        std::to_string(facts.version) + ", the host protocol version is " +
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

        const bool same_token = seen_hello && facts.token == hello.token;
        if (state == SessionState::kHandshaking && awaiting_topic && same_token) {
            // The MCU repeats HELLO while it hears nothing; the walk under way
            // is the answer to the first one, so a repeat changes nothing.
            return true;
        }
        hello = facts;
        seen_hello = true;
        // A HELLO that lands while connected is a reset, and the same HELLO
        // announces the new handshake, so no extra HELLO_REQ is needed
        // ([SES-19], [SES-21]).
        if (state == SessionState::kConnected) {
            ++resets;
        }
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
        std::uint8_t verdict = resolve ? resolve(info) : kTopicStatusOk;
        // The MCU stores the verdict per topic ([SES-2]); the host logs the
        // refusals, since the wire carries only the code.
        if (verdict == kTopicStatusUnknownType) {
            log(SessionLog::kError,
                "TOPIC refused: unknown type '" + info.type_name + "' for topic '" + info.topic_name + "'");
        } else if (verdict == kTopicStatusHashMismatch) {
            log(SessionLog::kError, "TOPIC refused: the hash of '" + info.type_name +
                                        "' is not the installed one, for topic '" + info.topic_name + "'");
        } else {
            // A name announced twice is one conflict, whatever the
            // directions ([CTL-13]): with one type the MCU would get its
            // own publications back through the bridge.
            for (const TopicInfo &seen : collected) {
                if (seen.topic_name == info.topic_name) {
                    verdict = kTopicStatusNameConflict;
                    log(SessionLog::kError,
                        "TOPIC refused: the name '" + info.topic_name + "' is announced twice");
                    break;
                }
            }
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
        if (topic_id < 16u || state != SessionState::kConnected) {
            // A later ticket's system channel, or traffic outside a session:
            // the frame still keeps the link alive.
            return;
        }
        // [FRM-15] A gap is not an error and does not drop the frame; it only
        // counts here, per topic, for the H8 diagnostics. The first frame
        // after a handshake expects seq 0.
        const std::uint8_t seq = frame.data[1];
        const auto expected = expected_seq.find(topic_id);
        const std::uint8_t want = expected == expected_seq.end() ? 0u : expected->second;
        gaps[topic_id] += static_cast<std::uint32_t>((seq - want) & 0xFFu);
        expected_seq[topic_id] = static_cast<std::uint8_t>(seq + 1u);
        for (const TopicInfo &topic : topics) {
            if (topic.topic_id == topic_id && topic.status == kTopicStatusOk && events.user_frame) {
                events.user_frame(topic_id, std::vector<std::uint8_t>(payload, payload + len));
                return;
            }
        }
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
    // The frames the link has not taken yet, oldest first, with their total
    // encoded size. The front frame may be partially written.
    std::deque<QueuedFrame> queue_;
    std::size_t pending_bytes_ = 0;
    std::size_t front_offset_ = 0;
    // Control frames a newer frame of the same slot replaced ([SES-33]).
    std::uint32_t control_coalesced_ = 0;

    SessionState state = SessionState::kDisconnected;
    std::int64_t next_open_ms = 0;
    std::int64_t boot_wait_deadline_ms = 0;
    std::int64_t next_hello_req_ms = 0;
    std::int64_t topic_deadline_ms = 0;
    std::int64_t next_heartbeat_ms = 0;
    std::int64_t last_rx_ms = 0;

    HelloFacts hello;
    bool seen_hello = false;
    bool awaiting_topic = false;
    std::uint8_t requested_index = 0;
    int retries = 0;

    std::vector<TopicInfo> collected;
    std::vector<TopicInfo> topics;
    bool had_handshake = false;
    std::uint32_t next_token = 0u;
    McuCounters mcu;
    std::uint32_t resets = 0;
    // The expected seq per MCU-to-host user topic, and the lost frames each
    // one skipped ([FRM-15]). The expectation restarts after each handshake,
    // the counts never do.
    std::map<std::uint8_t, std::uint8_t> expected_seq;
    std::map<std::uint8_t, std::uint32_t> gaps;

    std::uint8_t seq_control = 0;
    std::uint8_t seq_heartbeat = 0;
    // The next seq per host-to-MCU topic id, restarting at 0 after each
    // handshake ([FRM-4], [SES-11]).
    std::map<std::uint8_t, std::uint8_t> user_seq_;
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

bool Session::seen_hello() const
{
    return impl_->seen_hello;
}

std::uint32_t Session::token() const
{
    return impl_->hello.token;
}

const std::vector<TopicInfo> &Session::topics() const
{
    return impl_->topics;
}

McuCounters Session::mcu_counters() const
{
    return impl_->mcu;
}

std::uint32_t Session::seq_gaps(std::uint8_t topic_id) const
{
    const auto found = impl_->gaps.find(topic_id);
    return found == impl_->gaps.end() ? 0u : found->second;
}

std::uint32_t Session::resets() const
{
    return impl_->resets;
}

std::size_t Session::pending_bytes() const
{
    return impl_->pending_bytes_;
}

std::uint32_t Session::control_coalesced() const
{
    return impl_->control_coalesced_;
}

bool Session::send_user(std::uint8_t topic_id, const std::vector<std::uint8_t> &payload)
{
    const std::size_t cap = std::max<std::uint16_t>(impl_->hello.rx_capacity, impl_->hello.max_frame);
    if (impl_->pending_bytes_ > 0u && impl_->pending_bytes_ >= cap) {
        return false;
    }
    std::uint8_t seq = 0;
    const auto found = impl_->user_seq_.find(topic_id);
    if (found != impl_->user_seq_.end()) {
        seq = found->second;
    }
    impl_->queue_frame(topic_id, seq, payload);
    impl_->user_seq_[topic_id] = static_cast<std::uint8_t>(seq + 1u);
    impl_->flush();
    return true;
}

std::uint16_t Session::mcu_max_frame() const
{
    return impl_->seen_hello ? impl_->hello.max_frame : 0u;
}

std::uint16_t Session::mcu_rx_capacity() const
{
    return impl_->seen_hello ? impl_->hello.rx_capacity : 0u;
}

void Session::poll()
{
    impl_->poll();
}

} // namespace r2s_bridge
