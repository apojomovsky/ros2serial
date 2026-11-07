// The host session: the section 8.2 state machine. Timers advance only in
// poll(), the link is read through the core framer, and every frame the host
// sends is assembled here from the core's CRC and rCOBS.
#include "r2s_bridge/session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "r2s_bridge/converter.hpp"
#include "r2s_bridge/framing.hpp"
#include "r2s_bridge/time_sync.hpp"
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
constexpr std::uint8_t kTimeTopic = 1u;
constexpr std::uint8_t kLogTopic = 2u;
constexpr std::uint8_t kHeartbeatTopic = 4u;
constexpr std::uint8_t kErrorTopic = 5u;

// The parameters channel and the reliable-channel ack (sections 7.6, 7.7).
constexpr std::uint8_t kParamsTopic = 3u;
constexpr std::uint8_t kAckTopic = 6u;

// The parameters channel's opcodes ([PAR-1]).
constexpr std::uint8_t kParamReq = 0x01u;
constexpr std::uint8_t kParamDeclare = 0x02u;
constexpr std::uint8_t kParamSet = 0x03u;
constexpr std::uint8_t kParamStatus = 0x04u;

// The reliable engine ([REL-2]): one payload outstanding per direction,
// resent every 500 ms, dropped and counted after 3 retries.
constexpr std::int64_t kRelRetryMs = 500;
constexpr int kRelRetries = 3;

// The parameter walk ([PAR-7]) mirrors the topic walk ([SES-16]): one REQ
// per index, a 200 ms timeout, at most 3 retries. Bit 2 of the HELLO
// features announces the parameters ([CTL-2]).
constexpr std::uint8_t kParamFeaturesBit = 0x04u;
// Bits 6 and 7 announce services and actions, and only ride the reliable
// channel: a HELLO that sets either without bit 3 is refused ([CTL-17]).
constexpr std::uint8_t kReliableFeaturesBit = 0x08u;
constexpr std::uint8_t kSvcActFeaturesBits = 0xC0u;
constexpr std::int64_t kParamTimeoutMs = 200;
constexpr int kParamRetries = 3;

// The log channel's levels ([LOG-2]) and value kinds ([LOG-3]).
constexpr std::uint8_t kLogDebug = 10u;
constexpr std::uint8_t kLogInfo = 20u;
constexpr std::uint8_t kLogWarn = 30u;
constexpr std::uint8_t kLogError = 40u;
constexpr std::uint8_t kLogFatal = 50u;
constexpr std::uint8_t kLogNone = 0u;
constexpr std::uint8_t kLogU16 = 1u;
constexpr std::uint8_t kLogI16 = 2u;
constexpr std::uint8_t kLogU32 = 3u;
constexpr std::uint8_t kLogI32 = 4u;
constexpr std::uint8_t kLogF32 = 5u;

// The time channel's opcodes (spec/protocol.md, section 7.2).
constexpr std::uint8_t kPing = 0x01u;
constexpr std::uint8_t kPong = 0x02u;
constexpr std::uint8_t kCorrection = 0x03u;

// The time sync cadence ([SES-22], [SES-23]): the burst right after READY,
// the slower PINGs of the first 10 s, then the configured period.
constexpr std::int64_t kPingBurst = 8;
constexpr std::int64_t kPingBurstPeriodMs = 50;
constexpr std::int64_t kPingWarmPeriodMs = 1000;
constexpr std::int64_t kPingWarmWindowMs = 10000;
// A CORRECTION goes out at least this often ([SES-25]).
constexpr std::int64_t kCorrectionFloorNs = 5000000000;

// The spec's cadence ([SES-13] to [SES-18]).
constexpr std::int64_t kHelloReqPeriodMs = 500;
constexpr std::int64_t kTopicTimeoutMs = 200;
constexpr int kTopicRetries = 3;

// The handshake mode byte ([CTL-1]).
constexpr std::uint8_t kModeFull = 0u;

// Bits 6 and 7 are services and actions, so no features bit is reserved
// anymore; bit 4 says the MCU's `double` is 32 bits and bit 5 runtime
// registration, both informational ([CTL-2]). There is nothing for [CTL-3]
// to refuse, so the check is gone with the mask; the bridge test pins that
// every features byte is accepted.

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

// The slot a control payload competes for: one per control kind, one
// TOPIC_STATUS per topic id, the field the verdict carries on the wire, and
// one for the time channel's PINGs.
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
    Impl(SessionConfig in_config, const Clock &in_clock, const Clock &in_ros_clock, TransportFactory in_open,
         TopicResolver in_resolve, SessionEvents in_events)
        : config(in_config), clock(in_clock), ros_clock(in_ros_clock), open(std::move(in_open)),
          resolve(std::move(in_resolve)), events(std::move(in_events))
    {
        if (config.reconnect_period_ms <= 0 || config.boot_wait_ms <= 0 || config.heartbeat_period_ms <= 0 ||
            config.link_timeout_ms <= 0) {
            throw std::invalid_argument("every session period must be positive");
        }
        if (config.time_sync_period_ms < 1 || config.time_sync_period_ms > 5000) {
            throw std::invalid_argument("r2s_bridge: time_sync.period_ms must be between 1 and 5000, got " +
                                        std::to_string(config.time_sync_period_ms));
        }
        next_open_ms = now_ms();
        try_open();
    }

    std::int64_t now_ms() const
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock.now()).count();
    }

    std::int64_t now_ns() const
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(clock.now()).count();
    }

    std::int64_t ros_now_ns() const
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(ros_clock.now()).count();
    }

    void log(SessionLog level, const std::string &text)
    {
        if (events.log) {
            events.log(level, text);
        }
    }

    void mcu_log(SessionLog level, const std::string &text)
    {
        if (events.mcu_log) {
            events.mcu_log(level, text);
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
        reader = std::make_unique<FrameReader>(*transport, clock);
        reader->clear_link_fault();
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
        reset_time();
        state = SessionState::kDisconnected;
        next_open_ms = now_ms() + config.reconnect_period_ms;
    }

    // The estimator and the schedule start over: a correction anchored the
    // old handshake's ticks, and a PONG still in flight answers its PINGs.
    void reset_time()
    {
        sync.reset();
        time_supported_ = false;
        pings_sent = 0;
        ping_outstanding = false;
        correction_sent_ = false;
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
    void queue_frame(std::uint8_t topic_id, std::uint8_t seq, const std::vector<std::uint8_t> &payload,
                     bool use_slot = true)
    {
        QueuedFrame frame;
        frame.topic_id = topic_id;
        std::size_t match = queue_.size();
        if (topic_id < 16u && use_slot) {
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

    // One PARAM_REQ for the walk's current index ([PAR-7]). The REQ joins
    // the [SES-33] slot list, so a blocked link keeps the newest with the
    // first seq; a timer retry is a new emission with the next seq.
    void send_param_req()
    {
        queue_frame(kParamsTopic, seq_params_, {kParamReq, param_index_});
        ++seq_params_;
        param_deadline_ms_ = now_ms() + kParamTimeoutMs;
    }

    // The walk after the topic walk ([PAR-7]): REQ per index while the MCU
    // announced parameters, READY at once when it did not.
    void start_param_walk()
    {
        awaiting_topic = false;
        awaiting_param_ = false;
        if ((hello.features & kParamFeaturesBit) == 0u) {
            finish_handshake();
            return;
        }
        awaiting_param_ = true;
        param_index_ = 0;
        param_retries_ = 0;
        send_param_req();
    }

    // One PARAM_SET emission with a fresh rel_seq: the index and the value
    // travel after the opcode and rel_seq ([PAR-1]). Plain queueing, never
    // a slot: pending SETs replace at the API level instead ([REL-7]).
    void emit_set(std::uint8_t index, const std::vector<std::uint8_t> &value, std::uint8_t rel_seq)
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(value.size() + 3u);
        payload.push_back(kParamSet);
        payload.push_back(rel_seq);
        payload.push_back(index);
        payload.insert(payload.end(), value.begin(), value.end());
        queue_frame(kParamsTopic, seq_params_, payload, false);
        ++seq_params_;
        flush();
    }

    // One ack naming the channel and rel_seq of a reliable frame ([REL-1]).
    // The ack joins the slot list, so a blocked link keeps one pending ack
    // per channel with the first seq, each replaced frame counted ([REL-6]).
    void send_ack_frame(std::uint8_t channel, std::uint8_t rel_seq)
    {
        queue_frame(kAckTopic, seq_ack_, {channel, rel_seq});
        ++seq_ack_;
        flush();
    }

    // The oldest pending SET becomes outstanding with the next rel_seq and
    // goes out at once ([REL-7]).
    void promote_pending()
    {
        if (rel_pending_.empty()) {
            return;
        }
        const auto next = rel_pending_.begin();
        const std::uint8_t rel_seq = rel_next_++;
        emit_set(next->first, next->second, rel_seq);
        rel_out_active_ = true;
        rel_out_rel_ = rel_seq;
        rel_out_index_ = next->first;
        rel_out_value_ = next->second;
        rel_out_retries_ = 0;
        rel_out_deadline_ms_ = now_ms() + kRelRetryMs;
        rel_pending_.erase(next);
    }

    // A re-handshake drops the reliable state ([REL-5]): the outstanding
    // SET, the pending ones and their queued frames, even a partially
    // written one. Bytes already on the wire get a closing delimiter, so
    // the next frame decodes instead of merging into the stale prefix,
    // and a stale SET never completes under a new handshake's numbering.
    // Queued acks are control frames, dropped with the rest ([SES-33]).
    void drop_reliable()
    {
        rel_out_active_ = false;
        rel_pending_.clear();
        const bool drop_front =
            !queue_.empty() && queue_.front().topic_id == kParamsTopic && !queue_.front().control;
        std::deque<QueuedFrame> kept;
        std::size_t kept_bytes = 0;
        for (std::size_t i = 0; i < queue_.size(); ++i) {
            if (queue_[i].topic_id == kParamsTopic && !queue_[i].control) {
                continue;
            }
            kept_bytes += queue_[i].bytes.size();
            kept.push_back(std::move(queue_[i]));
        }
        queue_ = std::move(kept);
        pending_bytes_ = kept_bytes;
        if (drop_front) {
            // A bare delimiter ends the stale prefix on the wire, so the
            // next frame decodes instead of merging into it. The closed
            // fragment fails its check and counts once ([FRM-8]).
            if (front_offset_ > 0) {
                QueuedFrame delimiter;
                delimiter.bytes.push_back(0x00u);
                queue_.push_front(std::move(delimiter));
                ++pending_bytes_;
            }
            front_offset_ = 0;
        }
    }

    void send_heartbeat()
    {
        queue_frame(kHeartbeatTopic, seq_heartbeat, {});
        ++seq_heartbeat;
        next_heartbeat_ms = now_ms() + config.heartbeat_period_ms;
    }

    void put_le(std::vector<std::uint8_t> &payload, std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8) {
            payload.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFu));
        }
    }

    void send_ping()
    {
        // The send time stamps the sample; the frame carries the id the PONG
        // answers with ([TIM-2]). One slot holds the PINGs ([SES-33]), so a
        // wedged link keeps the newest id with the first seq.
        last_ping_send_ns_ = now_ns();
        last_ping_id_ = ping_id;
        std::vector<std::uint8_t> ping;
        ping.reserve(2u);
        ping.push_back(kPing);
        ping.push_back(ping_id);
        queue_frame(kTimeTopic, seq_time, ping);
        ++seq_time;
        ++ping_id;
        ++pings_sent;
        ping_outstanding = true;
    }

    void send_correction(const TimeCorrection &correction)
    {
        std::vector<std::uint8_t> payload;
        payload.reserve(15u);
        payload.push_back(kCorrection);
        put_le(payload, correction.tick0);
        put_le(payload, static_cast<std::uint32_t>(correction.sec));
        put_le(payload, correction.nanosec);
        const auto drift = static_cast<std::uint16_t>(correction.drift_ppm);
        payload.push_back(static_cast<std::uint8_t>(drift & 0xFFu));
        payload.push_back(static_cast<std::uint8_t>((drift >> 8u) & 0xFFu));
        queue_frame(kTimeTopic, seq_time, payload);
        ++seq_time;
    }

    void maybe_send_ping(std::int64_t now)
    {
        if (!config.time_sync_enabled || !sync || !time_supported_) {
            return;
        }
        if (now < next_ping_ms_) {
            return;
        }
        if (pings_sent >= kPingBurst) {
            // The burst is over: one PING every second for the first 10 s,
            // then every configured period ([SES-23]).
            send_ping();
            const std::int64_t age = now - connected_ms_;
            next_ping_ms_ = now + (age < kPingWarmWindowMs ? kPingWarmPeriodMs : config.time_sync_period_ms);
            return;
        }
        send_ping();
        next_ping_ms_ = now + (pings_sent < kPingBurst ? kPingBurstPeriodMs : kPingWarmPeriodMs);
    }

    void begin_handshake_wait()
    {
        state = SessionState::kHandshaking;
        next_hello_req_ms = now_ms();
    }

    void restart_handshake()
    {
        awaiting_topic = false;
        awaiting_param_ = false;
        requested_index = 0;
        retries = 0;
        collected.clear();
        clear_pending_control();
        drop_reliable();
        reset_time();
        begin_handshake_wait();
    }

    void start_topic_walk()
    {
        requested_index = 0;
        retries = 0;
        collected.clear();
        awaiting_topic = true;
        awaiting_param_ = false;
        clear_pending_control();
        drop_reliable();
        // A re-handshake drops the reliable state ([REL-5]); the walk and
        // the connected stream number their channel frames from 0.
        rel_next_ = 0;
        rel_expected_.clear();
        seq_params_ = 0;
        seq_ack_ = 0;
        if (requested_index < hello.topic_count) {
            send_topic_req();
        } else {
            start_param_walk();
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
        // The walk's REQs and the connected SETs number separately: the
        // vectors show the first SET at frame seq 0 after the walk.
        seq_control = 0;
        seq_heartbeat = 0;
        seq_params_ = 0;
        seq_ack_ = 0;
        user_seq_.clear();
        expected_seq.clear();
        last_rx_ms = now_ms();
        next_heartbeat_ms = now_ms() + config.heartbeat_period_ms;
        // The burst starts with READY: the first PING leaves in the same
        // poll ([SES-22]). The HELLO already proved the tick form ([CTL-8]).
        reset_time();
        sync.emplace(hello.tick_hz);
        time_supported_ = (hello.features & 0x01u) != 0u;
        connected_ms_ = now_ms();
        next_ping_ms_ = connected_ms_;
        ping_id = 0;
        seq_time = 0;
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
        if ((facts.features & kSvcActFeaturesBits) != 0u && (facts.features & kReliableFeaturesBit) == 0u) {
            log(SessionLog::kError, "HELLO refused: services without the reliable bit");
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
            start_param_walk();
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

    // The parameters channel ([PAR-7], [PAR-8], [REL-3]). Walk answers
    // arrive while handshaking and are never acked; STATUS applies at most
    // once while connected, acked every time and reported to the
    // parameters layer through the param_status event.
    void on_params(const std::uint8_t *payload, std::uint16_t len)
    {
        if (len < 1u) {
            return;
        }
        switch (payload[0]) {
        case kParamDeclare:
            // A DECLARE moves the walk to the next index. Its content is
            // the parameters layer's (X5), and it is never acked ([PAR-8]).
            if (state == SessionState::kHandshaking && awaiting_param_ && len >= 2u &&
                payload[1] == param_index_) {
                ++param_index_;
                param_retries_ = 0;
                send_param_req();
            }
            break;
        case kParamStatus:
            on_status(payload, len);
            break;
        default:
            break;
        }
    }

    void on_status(const std::uint8_t *payload, std::uint16_t len)
    {
        if (len < 4u || payload[3] > 4u) {
            return;
        }
        const std::uint8_t rel_seq = payload[1];
        const std::uint8_t index = payload[2];
        const std::uint8_t status = payload[3];
        if (state == SessionState::kHandshaking && awaiting_param_) {
            // A plain reply under the requester's retry ([PAR-8]).
            if (index != param_index_) {
                return;
            }
            if (status == 1u) {
                awaiting_param_ = false;
                finish_handshake();
            } else {
                ++param_index_;
                param_retries_ = 0;
                send_param_req();
            }
            return;
        }
        if (state != SessionState::kConnected) {
            return;
        }
        send_ack_frame(kParamsTopic, rel_seq);
        std::uint8_t &expected = rel_expected_[kParamsTopic];
        if (rel_seq == expected) {
            ++expected;
            if (events.param_status) {
                events.param_status(index, status);
            }
        }
    }

    // An ack for the outstanding SET clears it and promotes the oldest
    // pending one; anything else is a duplicate or a stranger ([REL-2]).
    void on_ack_frame(const std::uint8_t *payload, std::uint16_t len)
    {
        if (state != SessionState::kConnected || len != 2u || payload[0] != kParamsTopic ||
            !rel_out_active_ || payload[1] != rel_out_rel_) {
            return;
        }
        rel_out_active_ = false;
        promote_pending();
    }

    void maybe_send_correction(std::int64_t now)
    {
        if (!sync || !sync->has_estimate()) {
            return;
        }
        TimeCorrection fresh;
        if (!sync->correction(fresh)) {
            return;
        }
        if (correction_sent_) {
            // The floor resends on schedule; otherwise only a move past the
            // threshold since the last correction earns a new one ([SES-25]).
            if (now - last_correction_ns_ < kCorrectionFloorNs && !sync->moved_since(last_correction_)) {
                return;
            }
        } else if (pings_sent < kPingBurst) {
            // The first correction waits for the burst ([SES-25]).
            return;
        }
        send_correction(fresh);
        last_correction_ = fresh;
        last_correction_ns_ = now;
        correction_sent_ = true;
    }

    void on_time(const std::uint8_t *payload, std::uint16_t len)
    {
        if (state != SessionState::kConnected || !config.time_sync_enabled || !sync) {
            return;
        }
        // A PONG is the id it answers plus the delimiter tick ([TIM-2]).
        // Anything else on the channel is a later ticket's system frame.
        if (len != 6u || payload[0] != kPong) {
            return;
        }
        if (!ping_outstanding || payload[1] != last_ping_id_) {
            return;
        }
        ping_outstanding = false;
        Reader reader(payload + 2u, 4u);
        std::uint32_t tick = 0;
        if (!reader.u32(tick) || !reader.at_end()) {
            return;
        }
        const std::int64_t now = now_ns();
        sync->add_sample(last_ping_send_ns_, now, ros_now_ns(), tick);
        maybe_send_correction(now);
    }

    // A log line ([LOG-1]): the level, the text, the value kind and the
    // value. The line carries the value after the text, since a log sink
    // takes one string; floats render with 9 significant digits, which
    // round-trips a 32-bit value. A short payload, trailing bytes, a
    // reserved kind ([LOG-4]) or an unknown level refuses the frame
    // ([SYS-5]). Lines decode in any state, so boot lines are not lost.
    void on_log(const std::uint8_t *payload, std::uint16_t len)
    {
        Reader reader(payload, len);
        std::uint8_t level = 0;
        std::string text;
        std::uint8_t kind = 0;
        if (!reader.u8(level) || !reader.string(text) || !reader.u8(kind)) {
            return;
        }
        // A NUL byte never belongs in a string ([ENC-14]); the MCU holds
        // strings NUL-terminated, so one truncates the line for its reader.
        if (text.find('\0') != std::string::npos) {
            return;
        }
        std::string line = text;
        switch (kind) {
        case kLogNone:
            if (!reader.at_end()) {
                return;
            }
            break;
        case kLogU16: {
            std::uint16_t value = 0;
            if (!reader.u16(value) || !reader.at_end()) {
                return;
            }
            line += std::to_string(value);
            break;
        }
        case kLogI16: {
            std::uint16_t raw = 0;
            if (!reader.u16(raw) || !reader.at_end()) {
                return;
            }
            line += std::to_string(static_cast<std::int16_t>(raw));
            break;
        }
        case kLogU32: {
            std::uint32_t value = 0;
            if (!reader.u32(value) || !reader.at_end()) {
                return;
            }
            line += std::to_string(value);
            break;
        }
        case kLogI32: {
            std::uint32_t raw = 0;
            if (!reader.u32(raw) || !reader.at_end()) {
                return;
            }
            line += std::to_string(static_cast<std::int32_t>(raw));
            break;
        }
        case kLogF32: {
            std::uint32_t bits = 0;
            if (!reader.u32(bits) || !reader.at_end()) {
                return;
            }
            float value = 0.0F;
            std::memcpy(&value, &bits, sizeof value);
            char rendered[24];
            (void)std::snprintf(rendered, sizeof rendered, "%.9g", static_cast<double>(value));
            line += rendered;
            break;
        }
        default:
            return;
        }
        switch (level) {
        case kLogDebug:
            mcu_log(SessionLog::kDebug, line);
            break;
        case kLogInfo:
            mcu_log(SessionLog::kInfo, line);
            break;
        case kLogWarn:
            mcu_log(SessionLog::kWarning, line);
            break;
        case kLogError:
            mcu_log(SessionLog::kError, line);
            break;
        case kLogFatal:
            mcu_log(SessionLog::kFatal, line);
            break;
        default:
            break;
        }
    }

    // An error report ([ERR-1], [ERR-2]): the code, the topic, the count
    // and the field. Reports decode in any state like log lines: a code 5
    // arrives while the link is not connected. A short payload or an
    // unknown code refuses the frame ([SYS-5]). The topic names its
    // announcement when the handshake knows it, and a code 1 field its
    // introspected path ([ERR-5]); otherwise both stay numeric.
    void on_error(const std::uint8_t *payload, std::uint16_t len)
    {
        Reader reader(payload, len);
        std::uint8_t code = 0;
        std::uint8_t topic = 0;
        std::uint16_t count = 0;
        std::uint8_t field = 0;
        if (!reader.u8(code) || !reader.u8(topic) || !reader.u16(count) || !reader.u8(field) ||
            !reader.at_end()) {
            return;
        }
        if (code < 1u || code > 6u) {
            return;
        }
        error_sums_[code - 1u] += count;
        if (topic != 0xFFu) {
            error_by_topic_[topic][code - 1u] += count;
        }
        std::string topic_text = std::to_string(topic);
        std::string field_text;
        for (const TopicInfo &info : topics) {
            if (info.topic_id == topic) {
                topic_text = "'" + info.topic_name + "'";
                if (code == 1u && field != 0xFFu) {
                    field_text = converter_.field_path(info.type_name, field);
                    if (field_text.empty()) {
                        field_text = "#";
                        field_text += std::to_string(field);
                    }
                }
                break;
            }
        }
        std::string line = "mcu error report code " + std::to_string(code) + " on topic " + topic_text +
                           ": " + std::to_string(count) + (count == 1u ? " occurrence" : " occurrences");
        if (!field_text.empty()) {
            line += " at field '" + field_text + "'";
        }
        mcu_log(SessionLog::kWarning, line);
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
        if (topic_id == kTimeTopic) {
            on_time(payload, len);
            return;
        }
        if (topic_id == kLogTopic) {
            // Boot lines arrive before READY, so any state decodes them.
            on_log(payload, len);
            return;
        }
        if (topic_id == kErrorTopic) {
            on_error(payload, len);
            return;
        }
        if (topic_id == kParamsTopic) {
            on_params(payload, len);
            return;
        }
        if (topic_id == kAckTopic) {
            on_ack_frame(payload, len);
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

    // The outstanding SET's retry ([REL-2]): the same rel_seq under the
    // next frame seq. Nothing unsent may wait for the link: a blocked link
    // holds the retry without spending one, like a frame the link never
    // carried. Spent retries drop the SET, count it and promote the next.
    void poll_reliable(std::int64_t now)
    {
        if (!rel_out_active_ || now < rel_out_deadline_ms_) {
            return;
        }
        if (!queue_.empty()) {
            return;
        }
        if (rel_out_retries_ >= kRelRetries) {
            log(SessionLog::kWarning, "no ack for the PARAM_SET; the SET is dropped");
            ++reliable_dropped_;
            rel_out_active_ = false;
            promote_pending();
            return;
        }
        ++rel_out_retries_;
        rel_out_deadline_ms_ = now + kRelRetryMs;
        emit_set(rel_out_index_, rel_out_value_, rel_out_rel_);
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
            } else if (awaiting_param_) {
                if (now >= param_deadline_ms_) {
                    if (param_retries_ >= kParamRetries) {
                        log(SessionLog::kWarning, "no PARAM_DECLARE answered; the handshake starts over");
                        restart_handshake();
                        send_hello_req();
                    } else {
                        ++param_retries_;
                        send_param_req();
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
            } else {
                if (now >= next_heartbeat_ms) {
                    send_heartbeat();
                }
                poll_reliable(now);
                maybe_send_ping(now);
                // The correction floor needs no new PONG: with the estimate
                // standing still the timer resends it on schedule ([SES-25]).
                if (correction_sent_) {
                    maybe_send_correction(now_ns());
                }
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
            // A close inside the loop already dropped the reader.
            if (reader && reader->link_faulted()) {
                close_link();
                return;
            }
        }
        run_timers();
        flush();
    }

    SessionConfig config;
    const Clock &clock;
    const Clock &ros_clock;
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
    // The PING schedule: the handshake's time for the warm window, the next
    // due PING, and how many of the burst went out ([SES-22], [SES-23]).
    std::int64_t connected_ms_ = 0;
    std::int64_t next_ping_ms_ = 0;
    std::int64_t pings_sent = 0;
    // Whether the MCU announced time support ([CTL-2] bit 0). Without it the
    // host sends no PING: the firmware could never answer one.
    bool time_supported_ = false;
    bool ping_outstanding = false;
    std::uint8_t ping_id = 0;
    std::uint8_t last_ping_id_ = 0;
    std::int64_t last_ping_send_ns_ = 0;
    // The estimator behind the CORRECTIONs, alive only while connected, and
    // the last correction sent for the move and floor checks ([SES-25]).
    std::optional<TimeSync> sync;
    bool correction_sent_ = false;
    TimeCorrection last_correction_{};
    std::int64_t last_correction_ns_ = 0;

    HelloFacts hello;
    bool seen_hello = false;
    bool awaiting_topic = false;
    std::uint8_t requested_index = 0;
    int retries = 0;
    // The parameter walk ([PAR-7]): whether the topic walk handed over,
    // the index under request, its retries and its deadline.
    bool awaiting_param_ = false;
    std::uint8_t param_index_ = 0;
    int param_retries_ = 0;
    std::int64_t param_deadline_ms_ = 0;
    // The reliable engine ([REL-2], [REL-7]): one outstanding SET with its
    // timer and retries, one pending SET per index, the next rel_seq and
    // the per-channel expected rel_seq, with the giveup and replace counts.
    // Plain fields behind an active flag: the engaged check is one branch,
    // and every use reads the same shape the core keeps.
    bool rel_out_active_ = false;
    std::uint8_t rel_out_rel_ = 0;
    std::uint8_t rel_out_index_ = 0;
    std::vector<std::uint8_t> rel_out_value_;
    int rel_out_retries_ = 0;
    std::int64_t rel_out_deadline_ms_ = 0;
    std::map<std::uint8_t, std::vector<std::uint8_t>> rel_pending_;
    std::uint8_t rel_next_ = 0;
    std::map<std::uint8_t, std::uint8_t> rel_expected_;
    std::uint32_t reliable_dropped_ = 0;
    std::uint32_t reliable_replaced_ = 0;

    std::vector<TopicInfo> collected;
    std::vector<TopicInfo> topics;
    Converter converter_;
    bool had_handshake = false;
    std::uint32_t next_token = 0u;
    McuCounters mcu;
    std::uint32_t resets = 0;
    // The expected seq per MCU-to-host user topic, and the lost frames each
    // one skipped ([FRM-15]). The expectation restarts after each handshake,
    // the counts never do.
    std::map<std::uint8_t, std::uint8_t> expected_seq;
    std::map<std::uint8_t, std::uint32_t> gaps;
    // The running error report sums per code ([ERR-6]), and the same counts
    // per naming topic id. A report adds its count to its code's sum, and
    // to its topic's slot unless it names no topic (0xFF). The sums never
    // wrap; unreported codes and ids read zero.
    std::array<std::uint64_t, 6> error_sums_{};
    std::map<std::uint8_t, std::array<std::uint64_t, 6>> error_by_topic_;

    std::uint8_t seq_control = 0;
    std::uint8_t seq_heartbeat = 0;
    // The frame seqs of the parameters channel and the ack channel,
    // restarting at each handshake and at READY ([FRM-4], [REL-5]).
    std::uint8_t seq_params_ = 0;
    std::uint8_t seq_ack_ = 0;
    // The seq the time channel shares between its PINGs and CORRECTIONs,
    // restarting at 0 after each handshake ([FRM-4], [FRM-5]).
    std::uint8_t seq_time = 0;
    // The next seq per host-to-MCU topic id, restarting at 0 after each
    // handshake ([FRM-4], [SES-11]).
    std::map<std::uint8_t, std::uint8_t> user_seq_;
};

Session::Session(SessionConfig config, const Clock &clock, const Clock &ros_clock, TransportFactory open,
                 TopicResolver resolve, SessionEvents events)
    : impl_(std::make_unique<Impl>(config, clock, ros_clock, std::move(open), std::move(resolve),
                                   std::move(events)))
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

std::array<std::uint64_t, 6> Session::error_sums() const
{
    return impl_->error_sums_;
}

std::array<std::uint64_t, 6> Session::error_counts(std::uint8_t topic_id) const
{
    const auto found = impl_->error_by_topic_.find(topic_id);
    return found == impl_->error_by_topic_.end() ? std::array<std::uint64_t, 6>{} : found->second;
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

bool Session::time_available() const
{
    return impl_->correction_sent_ && impl_->sync.has_value() && impl_->sync->has_estimate();
}

std::int64_t Session::time_offset_ns() const
{
    return time_available() ? impl_->sync->offset_ns() : 0;
}

std::int16_t Session::time_drift_ppm() const
{
    return time_available() ? impl_->sync->drift_ppm() : 0;
}

std::int64_t Session::time_round_trip_ns() const
{
    return time_available() ? impl_->sync->round_trip_ns() : 0;
}

std::size_t Session::pending_bytes() const
{
    return impl_->pending_bytes_;
}

std::uint32_t Session::control_coalesced() const
{
    return impl_->control_coalesced_;
}

bool Session::send_param_set(std::uint8_t index, const std::vector<std::uint8_t> &value)
{
    Impl &impl = *impl_;
    if (impl.state != SessionState::kConnected) {
        return false;
    }
    const std::size_t cap = std::max<std::uint16_t>(impl.hello.rx_capacity, impl.hello.max_frame);
    if (impl.pending_bytes_ > 0u && impl.pending_bytes_ >= cap) {
        return false;
    }
    if (!impl.rel_out_active_) {
        const std::uint8_t rel_seq = impl.rel_next_++;
        impl.emit_set(index, value, rel_seq);
        impl.rel_out_active_ = true;
        impl.rel_out_rel_ = rel_seq;
        impl.rel_out_index_ = index;
        impl.rel_out_value_ = value;
        impl.rel_out_retries_ = 0;
        impl.rel_out_deadline_ms_ = impl.now_ms() + kRelRetryMs;
    } else {
        const auto found = impl.rel_pending_.find(index);
        if (found != impl.rel_pending_.end()) {
            found->second = value;
            ++impl.reliable_replaced_;
        } else {
            impl.rel_pending_.emplace(index, value);
        }
    }
    return true;
}

std::uint32_t Session::reliable_dropped() const
{
    return impl_->reliable_dropped_;
}

std::uint32_t Session::reliable_replaced() const
{
    return impl_->reliable_replaced_;
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
