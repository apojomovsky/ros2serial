// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The host session: the section 8.2 state machine. Timers advance only in
// poll(), the link is read through the core framer, and every frame the host
// sends is assembled here from the core's CRC and rCOBS.
#include "r2s_bridge/session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <optional>
#include <set>
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

// The services channel id and its opcodes (spec/protocol.md, 7.8).
constexpr std::uint8_t kServicesTopic = 7u;
constexpr std::uint8_t kSvcQuery = 0x01u;
constexpr std::uint8_t kSvcDeclare = 0x02u;
constexpr std::uint8_t kSvcStatus = 0x03u;
constexpr std::uint8_t kSvcRequest = 0x04u;
constexpr std::uint8_t kSvcResponse = 0x05u;
constexpr std::uint8_t kSvcFeaturesBit = 0x40u;
// The busiest RESPONSE status section 7.8 names; anything above is
// reserved.
constexpr std::uint8_t kSvcBusy = 0x03u;

// The service walk's cadence, mirroring the parameter walk ([SVR-7]).
constexpr std::int64_t kSvcTimeoutMs = 200;
constexpr int kSvcRetries = 3;

// The actions channel id and its opcodes (spec/protocol.md, 7.9).
constexpr std::uint8_t kActionsTopic = 8u;
constexpr std::uint8_t kActQuery = 0x01u;
constexpr std::uint8_t kActDeclare = 0x02u;
constexpr std::uint8_t kActStatus = 0x03u;
constexpr std::uint8_t kActGoal = 0x04u;
constexpr std::uint8_t kActGoalResp = 0x05u;
constexpr std::uint8_t kActCancel = 0x06u;
constexpr std::uint8_t kActCancelResp = 0x07u;
constexpr std::uint8_t kActFeedback = 0x08u;
constexpr std::uint8_t kActResult = 0x09u;
constexpr std::uint8_t kActFeaturesBit = 0x80u;
// The GOAL_RESP decisions ([ACT-4]), the CANCEL_RESP decisions ([ACT-5])
// and the RESULT outcomes ([ACT-6]): anything else is reserved.
constexpr std::uint8_t kActAccepted = 0x00u;
constexpr std::uint8_t kActRejected = 0x01u;
constexpr std::uint8_t kActBusy = 0x02u;
constexpr std::uint8_t kActUnknownAction = 0x03u;
constexpr std::uint8_t kActCancelAccepted = 0x00u;
constexpr std::uint8_t kActCancelRejected = 0x01u;
constexpr std::uint8_t kActUnknownGoal = 0x02u;
constexpr std::uint8_t kActSucceeded = 0x04u;
constexpr std::uint8_t kActCanceled = 0x05u;
constexpr std::uint8_t kActAborted = 0x06u;

// The action walk's cadence, mirroring the service walk ([ACT-7]).
constexpr std::int64_t kActTimeoutMs = 200;
constexpr int kActRetries = 3;

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
struct QueuedFrame
{
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
QueuedFrame control_slot(std::uint8_t topic_id, const std::vector<std::uint8_t> & payload)
{
  QueuedFrame slot;
  slot.control = true;
  slot.topic_id = topic_id;
  if (!payload.empty()) {
    slot.opcode = payload[0];
    if (topic_id == kControlTopic && payload[0] == kTopicStatus && payload.size() > 1u) {
      slot.sub = payload[1];
    }
    if (topic_id == kServicesTopic && payload[0] == kSvcStatus && payload.size() > 2u) {
      slot.sub = payload[2];
    }
    if (topic_id == kActionsTopic && payload[0] == kActQuery && payload.size() > 1u) {
      slot.sub = payload[1];
    }
  }
  return slot;
}

// The protocol's own frame limit ([FRM-9]).
constexpr std::uint32_t kMaxFrameLimit = 65277u;

// The payload a HELLO carries, after its opcode ([CTL-16]).
struct HelloFacts
{
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
  Reader(const std::uint8_t *data, std::uint16_t len)
  : data_(data), len_(len)
  {
  }

  bool u8(std::uint8_t & out)
  {
    if (left() < 1u) {
      return false;
    }
    out = data_[pos_++];
    return true;
  }

  bool u16(std::uint16_t & out)
  {
    if (left() < 2u) {
      return false;
    }
    out = static_cast<std::uint16_t>(data_[pos_] | (data_[pos_ + 1u] << 8u));
    pos_ += 2u;
    return true;
  }

  bool u32(std::uint32_t & out)
  {
    if (left() < 4u) {
      return false;
    }
    out = static_cast<std::uint32_t>(data_[pos_]) | (static_cast<std::uint32_t>(data_[pos_ + 1u]) <<
      8u) |
      (static_cast<std::uint32_t>(data_[pos_ + 2u]) << 16u) |
      (static_cast<std::uint32_t>(data_[pos_ + 3u]) << 24u);
    pos_ += 4u;
    return true;
  }

    // A varint length and its bytes, as [ENC-7] lays a string out.
  bool string(std::string & out)
  {
    std::uint32_t length = 0;
    std::uint32_t shift = 0;
    for (;; ) {
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

    // The bytes past what u8/u16/u32/string consumed, for a trailing value
    // the caller shapes itself.
  bool rest(std::vector<std::uint8_t> & out)
  {
    out.assign(data_ + pos_, data_ + len_);
    pos_ = len_;
    return true;
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

}  // namespace

struct Session::Impl
{
  Impl(
    SessionConfig in_config, const Clock & in_clock, const Clock & in_ros_clock,
    TransportFactory in_open,
    TopicResolver in_resolve, SessionEvents in_events, ServiceResolver in_resolve_service,
    ActionResolver in_resolve_action)
  : config(in_config), clock(in_clock), ros_clock(in_ros_clock), open(std::move(in_open)),
    resolve(std::move(in_resolve)), events(std::move(in_events)),
    resolve_service(std::move(in_resolve_service)),
    resolve_action(std::move(in_resolve_action))
  {
    if (config.reconnect_period_ms <= 0 || config.boot_wait_ms <= 0 ||
      config.heartbeat_period_ms <= 0 ||
      config.link_timeout_ms <= 0)
    {
      throw std::invalid_argument("every session period must be positive");
    }
    if (config.time_sync_period_ms < 1 || config.time_sync_period_ms > 5000) {
      throw std::invalid_argument(
          "r2s_bridge: time_sync.period_ms must be between 1 and 5000, got " +
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

  void log(SessionLog level, const std::string & text)
  {
    if (events.log) {
      events.log(level, text);
    }
  }

  void mcu_log(SessionLog level, const std::string & text)
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
  void queue_frame(
    std::uint8_t topic_id, std::uint8_t seq, const std::vector<std::uint8_t> & payload,
    bool use_slot = true)
  {
    QueuedFrame frame;
    frame.topic_id = topic_id;
    std::size_t match = queue_.size();
    if (topic_id < 16u && use_slot) {
      frame = control_slot(topic_id, payload);
      const std::size_t first = front_offset_ > 0u ? 1u : 0u;
      for (std::size_t i = first; i < queue_.size(); ++i) {
        const QueuedFrame & slot = queue_[i];
        if (slot.control && slot.topic_id == frame.topic_id && slot.opcode == frame.opcode &&
          slot.sub == frame.sub)
        {
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
      r2s_crc16_update_buf(R2S_CRC16_INIT, decoded.data(),
        static_cast<std::uint16_t>(decoded.size()));
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
      QueuedFrame & slot = queue_[match];
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
      QueuedFrame & front = queue_.front();
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

  void send_control(const std::vector<std::uint8_t> & payload)
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
    // announced parameters, the service walk at once when it did not.
  void start_param_walk()
  {
    awaiting_topic = false;
    awaiting_param_ = false;
    collected_params.clear();
    if ((hello.features & kParamFeaturesBit) == 0u) {
      start_service_walk();
      return;
    }
    awaiting_param_ = true;
    param_index_ = 0;
    param_retries_ = 0;
    send_param_req();
  }

    // One SVC_QUERY for the walk's current index ([SVR-7]). The QUERY joins
    // the [SES-33] slot list like PARAM_REQ, so a blocked link keeps the
    // newest with the first seq; a timer retry is a new emission with the
    // next seq.
  void send_svc_query()
  {
    queue_frame(kServicesTopic, seq_svc_, {kSvcQuery, svc_index_});
    ++seq_svc_;
    flush();
    svc_deadline_ms_ = now_ms() + kSvcTimeoutMs;
  }

    // The walk after the parameter walk ([SVR-7]): QUERY per index while the
    // MCU announced services, the action walk at once when it did not.
  void start_service_walk()
  {
    awaiting_param_ = false;
    awaiting_svc_ = false;
    collected_services.clear();
    if ((hello.features & kSvcFeaturesBit) == 0u) {
      start_action_walk();
      return;
    }
    awaiting_svc_ = true;
    svc_index_ = 0;
    svc_retries_ = 0;
    send_svc_query();
  }

    // One ACT_QUERY for the walk's current index ([ACT-7]). The QUERY joins
    // the [SES-33] slot list like SVC_QUERY, so a blocked link keeps the
    // newest with the first seq; a timer retry is a new emission with the
    // next seq.
  void send_act_query()
  {
    queue_frame(kActionsTopic, seq_act_, {kActQuery, act_index_});
    ++seq_act_;
    flush();
    act_deadline_ms_ = now_ms() + kActTimeoutMs;
  }

    // The walk after the service walk ([ACT-7]): QUERY per index while the
    // MCU announced actions, READY at once when it did not.
  void start_action_walk()
  {
    awaiting_svc_ = false;
    awaiting_act_ = false;
    collected_actions.clear();
    if ((hello.features & kActFeaturesBit) == 0u) {
      finish_handshake();
      return;
    }
    awaiting_act_ = true;
    act_index_ = 0;
    act_retries_ = 0;
    send_act_query();
  }

    // One PARAM_SET emission with a fresh rel_seq: the index and the value
    // travel after the opcode and rel_seq ([PAR-1]). Plain queueing, never
    // a slot: pending SETs replace at the API level instead ([REL-7]).
  void emit_set(std::uint8_t index, const std::vector<std::uint8_t> & value, std::uint8_t rel_seq)
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
    svc_out_active_ = false;
    svc_pending_requests_.clear();
    svc_pending_response_ = false;
    svc_mcu_req_active_ = false;
    svc_host_req_active_ = false;
    act_out_active_ = false;
    act_pending_goals_.clear();
    act_pending_cancel_ = false;
    act_host_goals_.clear();
    act_host_cancels_.clear();
    const bool drop_front =
      !queue_.empty() &&
      (queue_.front().topic_id == kParamsTopic || queue_.front().topic_id == kServicesTopic ||
      queue_.front().topic_id == kActionsTopic) &&
      !queue_.front().control;
    std::deque<QueuedFrame> kept;
    std::size_t kept_bytes = 0;
    for (std::size_t i = 0; i < queue_.size(); ++i) {
      if ((queue_[i].topic_id == kParamsTopic || queue_[i].topic_id == kServicesTopic ||
        queue_[i].topic_id == kActionsTopic) &&
        !queue_[i].control)
      {
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

  void put_le(std::vector<std::uint8_t> & payload, std::uint32_t value)
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

  void send_correction(const TimeCorrection & correction)
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
      next_ping_ms_ = now + (age <
        kPingWarmWindowMs ? kPingWarmPeriodMs : config.time_sync_period_ms);
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
    awaiting_svc_ = false;
    awaiting_act_ = false;
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
    awaiting_svc_ = false;
    awaiting_act_ = false;
    clear_pending_control();
    drop_reliable();
        // A re-handshake drops the reliable state ([REL-5]); the walk and
        // the connected stream number their channel frames from 0.
    rel_next_ = 0;
    rel_next_svc_ = 0;
    rel_next_act_ = 0;
    rel_expected_.clear();
    seq_params_ = 0;
    seq_svc_ = 0;
    seq_act_ = 0;
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
    params = collected_params;
    services_ = collected_services;
    actions_ = collected_actions;
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
    seq_svc_ = 0;
    seq_ack_ = 0;
        // The actions channel keeps numbering across READY: the walk's
        // QUERYs and the connected GOALs share one frame seq, as the MCU
        // core and the session vectors show (spec/vectors/session). Only
        // a re-handshake restarts it, in start_topic_walk.
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
      !reader.u16(facts.max_frame) || !reader.u16(facts.rx_capacity) ||
      !reader.u32(facts.tick_hz) ||
      !reader.u8(facts.features) || !reader.u8(facts.topic_count))
    {
      log(SessionLog::kError, "HELLO refused: the payload is shorter than its fields");
      return false;
    }
    if (facts.version != R2S_PROTOCOL_VERSION) {
      log(SessionLog::kError, "HELLO refused: the MCU protocol version is " +
                                        std::to_string(facts.version) +
          ", the host protocol version is " +
                                        std::to_string(R2S_PROTOCOL_VERSION));
      return false;
    }
    if (facts.mode != kModeFull) {
      log(SessionLog::kError,
                "HELLO refused: compact mode needs a node map, which is not supported yet");
      return false;
    }
    if ((facts.features & kSvcActFeaturesBits) != 0u &&
      (facts.features & kReliableFeaturesBit) == 0u)
    {
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
                                        std::to_string(info.topic_id) +
          " is not the one requested");
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
                "TOPIC refused: unknown type '" + info.type_name + "' for topic '" +
          info.topic_name + "'");
    } else if (verdict == kTopicStatusHashMismatch) {
      log(SessionLog::kError, "TOPIC refused: the hash of '" + info.type_name +
                                        "' is not the installed one, for topic '" +
          info.topic_name + "'");
    } else {
            // A name announced twice is one conflict, whatever the
            // directions ([CTL-13]): with one type the MCU would get its
            // own publications back through the bridge.
      for (const TopicInfo & seen : collected) {
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

    // One walk DECLARE kept for the parameters layer: the index, the PAR-2
    // type, the PAR-4 flags, the name and the default in its wire form.
    // Only a fully parseable DECLARE is kept: a short payload, an unknown
    // type, reserved flag bits, a name the length overruns, or a default
    // outside its type's shape ([PAR-3]) leaves no entry, while the walk
    // still advances.
  void collect_declare(const std::uint8_t *payload, std::uint16_t len)
  {
        // The caller only delivers walk answers of two bytes or more
        // ([PAR-8]): the opcode and the declare index always parse.
    ParamInfo info;
    info.index = payload[1];
    Reader reader(payload + 2u, static_cast<std::uint16_t>(len - 2u));
    std::string name;
    std::vector<std::uint8_t> def;
    if (!reader.u8(info.type) || !reader.u8(info.flags) || !reader.string(name)) {
      return;
    }
    reader.rest(def);
    if (info.type > 3u || (info.flags & 0xFEu) != 0u) {
      return;
    }
    if (info.type == 0u) {
      if (def.size() != 1u || def[0] > 1u) {
        return;
      }
    } else if (info.type == 1u || info.type == 2u) {
      if (def.size() != 8u) {
        return;
      }
    } else {
      Reader tail(def.data(), static_cast<std::uint16_t>(def.size()));
      std::string text;
      if (!tail.string(text) || !tail.at_end()) {
        return;
      }
      if (text.find('\0') != std::string::npos) {
        return;
      }
    }
    info.name = name;
    info.default_value = def;
    collected_params.push_back(info);
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
      case kParamDeclare: {
            // A DECLARE moves the walk to the next index and is never acked
            // ([PAR-8]). The parameters layer keeps a parseable one.
          if (state == SessionState::kHandshaking && awaiting_param_ && len >= 2u &&
            payload[1] == param_index_)
          {
            collect_declare(payload, len);
            ++param_index_;
            param_retries_ = 0;
            send_param_req();
          }
          break;
        }
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
        start_service_walk();
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
    std::uint8_t & expected = rel_expected_[kParamsTopic];
    if (rel_seq == expected) {
      ++expected;
      if (events.param_status) {
        events.param_status(index, status);
      }
    }
  }

    // One walk DECLARE kept for the services layer: the index, the SVR-2
    // role, the hash and the names. Only a fully parseable DECLARE is kept:
    // a short payload, an unknown role or names the length overruns leave
    // no entry, while the walk still advances ([SVR-1], [SVR-2]).
  void collect_svc_declare(const std::uint8_t *payload, std::uint16_t len)
  {
        // The caller only delivers walk answers of two bytes or more
        // ([SVR-7]): the opcode and the declare index always parse.
    ServiceInfo info;
    info.index = payload[1];
    Reader reader(payload + 2u, static_cast<std::uint16_t>(len - 2u));
    std::uint8_t role = 0;
    if (!reader.u8(role) || role > kServiceRoleClient) {
      return;
    }
    info.role = role;
    for (std::size_t i = 0; i < info.type_hash.size(); ++i) {
      std::uint8_t byte = 0;
      if (!reader.u8(byte)) {
        return;
      }
      info.type_hash[i] = byte;
    }
    if (!reader.string(info.type_name) || !reader.string(info.service_name) ||
      !reader.at_end())
    {
      return;
    }
    collected_services.push_back(info);
  }

    // One SVC_DECLARE answer ([SVR-7]): the entry is kept when it parses,
    // named with the resolver's verdict, and answered with STATUS. The walk
    // moves to the next index either way.
  void on_service_declare(const std::uint8_t *payload, std::uint16_t len)
  {
    const std::size_t kept = collected_services.size();
    collect_svc_declare(payload, len);
    std::uint8_t verdict = kServiceStatusUnknownType;
    if (collected_services.size() > kept) {
      ServiceInfo & info = collected_services.back();
      verdict = resolve_service ? resolve_service(info) : kServiceStatusUnknownType;
      if (verdict == kServiceStatusOk) {
        for (const ServiceInfo & seen : collected_services) {
          if (seen.index != info.index && seen.service_name == info.service_name) {
            verdict = kServiceStatusNameConflict;
            log(SessionLog::kError,
              "SVC refused: the name '" + info.service_name + "' is announced twice");
            break;
          }
        }
      } else if (verdict == kServiceStatusUnknownType) {
        log(SessionLog::kError,
          "SVC refused: unknown type '" + info.type_name + "' for service '" +
          info.service_name + "'");
      } else if (verdict == kServiceStatusHashMismatch) {
        log(SessionLog::kError,
          "SVC refused: the hash of '" + info.type_name +
          "' is not the installed one, for service '" +
          info.service_name + "'");
      }
      info.status = verdict;
    }
    const std::uint8_t index = payload[1];
    queue_frame(kServicesTopic, seq_svc_, {kSvcStatus, 0x00u, index, verdict});
    ++seq_svc_;
    flush();
    svc_index_ = static_cast<std::uint8_t>(index + 1u);
    svc_retries_ = 0;
    send_svc_query();
  }

    // The service walk's STATUS ([SVR-7]): 1 ends the walk at the index
    // under request, anything else advances it. Plain replies under the
    // requester's retry, never acked.
  void on_service_status(const std::uint8_t *payload, std::uint16_t len)
  {
    if (len != 4u) {
      return;
    }
    if (payload[2] != svc_index_) {
      return;
    }
    if (payload[3] == 1u) {
      awaiting_svc_ = false;
      start_action_walk();
    } else {
      ++svc_index_;
      svc_retries_ = 0;
      send_svc_query();
    }
  }

    // One walk DECLARE kept for the actions layer: the index, the hash and
    // the names. Only a fully parseable DECLARE is kept: a short payload
    // or names the length overruns leave no entry, while the walk still
    // advances ([ACT-1]).
  void collect_act_declare(const std::uint8_t *payload, std::uint16_t len)
  {
        // The caller only delivers walk answers of two bytes or more
        // ([ACT-7]): the opcode and the declare index always parse.
    ActionInfo info;
    info.index = payload[1];
    Reader reader(payload + 2u, static_cast<std::uint16_t>(len - 2u));
    for (std::size_t i = 0; i < info.type_hash.size(); ++i) {
      std::uint8_t byte = 0;
      if (!reader.u8(byte)) {
        return;
      }
      info.type_hash[i] = byte;
    }
    if (!reader.string(info.type_name) || !reader.string(info.action_name) ||
      !reader.at_end())
    {
      return;
    }
    collected_actions.push_back(info);
  }

    // One ACT_DECLARE answer ([ACT-7]): the entry is kept when it parses,
    // named with the resolver's verdict, and answered with STATUS. The walk
    // moves to the next index either way.
  void on_action_declare(const std::uint8_t *payload, std::uint16_t len)
  {
    const std::size_t kept = collected_actions.size();
    collect_act_declare(payload, len);
    std::uint8_t verdict = kActionStatusUnknownType;
    if (collected_actions.size() > kept) {
      ActionInfo & info = collected_actions.back();
      verdict = resolve_action ? resolve_action(info) : kActionStatusUnknownType;
      if (verdict == kActionStatusOk) {
        for (const ActionInfo & seen : collected_actions) {
          if (seen.index != info.index && seen.action_name == info.action_name) {
            verdict = kActionStatusNameConflict;
            log(SessionLog::kError,
              "ACT refused: the name '" + info.action_name + "' is announced twice");
            break;
          }
        }
      } else if (verdict == kActionStatusUnknownType) {
        log(SessionLog::kError,
          "ACT refused: unknown type '" + info.type_name + "' for action '" +
          info.action_name + "'");
      } else if (verdict == kActionStatusHashMismatch) {
        log(SessionLog::kError,
          "ACT refused: the hash of '" + info.type_name +
          "' is not the installed one, for action '" +
          info.action_name + "'");
      }
      info.status = verdict;
    }
    const std::uint8_t index = payload[1];
    queue_frame(kActionsTopic, seq_act_, {kActStatus, 0x00u, index, verdict});
    ++seq_act_;
    flush();
    act_index_ = static_cast<std::uint8_t>(index + 1u);
    act_retries_ = 0;
    send_act_query();
  }

    // The action walk's STATUS ([ACT-7]): 1 ends the walk at the index
    // under request, anything else advances it. Plain replies under the
    // requester's retry, never acked.
  void on_action_status(const std::uint8_t *payload, std::uint16_t len)
  {
    if (len != 4u) {
      return;
    }
    if (payload[2] != act_index_) {
      return;
    }
    if (payload[3] == 1u) {
      awaiting_act_ = false;
      finish_handshake();
    } else {
      ++act_index_;
      act_retries_ = 0;
      send_act_query();
    }
  }

    // One SVC_REQUEST emission with a fresh rel_seq: the index, the req_id
    // and the packed request travel after the opcode ([SVR-4]). Plain
    // queueing, never a slot: pending REQUESTs replace at the index level
    // instead ([SVR-10]).
    // One REQUEST emission: the frame goes out and the stream tracks it,
    // with the host's call under its req_id for the answer ([SVR-5]).
  void emit_svc_request(
    std::uint8_t index, std::uint8_t req, const std::vector<std::uint8_t> & payload,
    std::uint8_t rel_seq)
  {
    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 4u);
    frame.push_back(kSvcRequest);
    frame.push_back(rel_seq);
    frame.push_back(req);
    frame.push_back(index);
    frame.insert(frame.end(), payload.begin(), payload.end());
    svc_out_payload_ = frame;
    queue_frame(kServicesTopic, seq_svc_, frame, false);
    ++seq_svc_;
    flush();
    svc_out_active_ = true;
    svc_out_rel_ = rel_seq;
    svc_out_retries_ = 0;
    svc_out_deadline_ms_ = now_ms() + kRelRetryMs;
    svc_host_req_active_ = true;
    svc_host_req_ = req;
  }

    // One RESPONSE emission: the frame goes out and the MCU's tracked call
    // retires with it ([SVR-5]).
  void emit_svc_response(
    std::uint8_t req, std::uint8_t status, const std::vector<std::uint8_t> & payload,
    std::uint8_t rel_seq)
  {
    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 4u);
    frame.push_back(kSvcResponse);
    frame.push_back(rel_seq);
    frame.push_back(req);
    frame.push_back(status);
    frame.insert(frame.end(), payload.begin(), payload.end());
    svc_out_payload_ = frame;
    queue_frame(kServicesTopic, seq_svc_, frame, false);
    ++seq_svc_;
    flush();
    svc_out_active_ = true;
    svc_out_rel_ = rel_seq;
    svc_out_retries_ = 0;
    svc_out_deadline_ms_ = now_ms() + kRelRetryMs;
    svc_mcu_req_active_ = false;
  }

    // The pending answer goes out before the pending REQUESTs: it answers a
    // live call under its own timeout ([SVR-5]).
  void promote_svc_pending()
  {
    if (svc_pending_response_) {
      const std::uint8_t rel_seq = rel_next_svc_++;
      const std::uint8_t req = svc_pending_response_req_;
      const std::uint8_t status = svc_pending_response_status_;
      const std::vector<std::uint8_t> payload = svc_pending_response_payload_;
      svc_pending_response_ = false;
      emit_svc_response(req, status, payload, rel_seq);
      return;
    }
    if (svc_pending_requests_.empty()) {
      return;
    }
    const auto next = svc_pending_requests_.begin();
    const std::uint8_t req = next->second.req;
    const std::uint8_t index = next->first;
    const std::vector<std::uint8_t> payload = next->second.payload;
    svc_pending_requests_.erase(next);
    const std::uint8_t rel_seq = rel_next_svc_++;
    emit_svc_request(index, req, payload, rel_seq);
  }

    // The outstanding service frame's retry ([REL-2]): the same rel_seq
    // under the next frame seq. Nothing unsent may wait for the link: a
    // blocked link holds the retry without spending one, like a frame the
    // link never carried. Spent retries drop the frame, count it and
    // promote the next.
  void poll_svc_reliable(std::int64_t now)
  {
    if (!svc_out_active_ || now < svc_out_deadline_ms_) {
      return;
    }
    if (!queue_.empty()) {
      return;
    }
    if (svc_out_retries_ >= kRelRetries) {
      log(SessionLog::kWarning, "no ack for the service frame; the frame is dropped");
      ++reliable_dropped_;
      svc_out_active_ = false;
      promote_svc_pending();
      return;
    }
    ++svc_out_retries_;
    svc_out_deadline_ms_ = now + kRelRetryMs;
    queue_frame(kServicesTopic, seq_svc_, svc_out_payload_, false);
    ++seq_svc_;
    flush();
  }

    // One ACT_GOAL emission with a fresh rel_seq: the goal_seq, the index
    // and the packed goal travel after the opcode ([ACT-1]). Plain
    // queueing, never a slot: pending GOALs replace at the index level.
  void emit_act_goal(
    std::uint8_t index, std::uint8_t goal_seq, const std::vector<std::uint8_t> & payload,
    std::uint8_t rel_seq)
  {
    std::vector<std::uint8_t> frame;
    frame.reserve(payload.size() + 4u);
    frame.push_back(kActGoal);
    frame.push_back(rel_seq);
    frame.push_back(goal_seq);
    frame.push_back(index);
    frame.insert(frame.end(), payload.begin(), payload.end());
    act_out_payload_ = frame;
    queue_frame(kActionsTopic, seq_act_, frame, false);
    ++seq_act_;
    flush();
    act_out_active_ = true;
    act_out_rel_ = rel_seq;
    act_out_retries_ = 0;
    act_out_deadline_ms_ = now_ms() + kRelRetryMs;
    act_host_goals_.insert(goal_seq);
  }

    // One ACT_CANCEL emission with a fresh rel_seq: the goal_seq travels
    // after the opcode ([ACT-1]).
  void emit_act_cancel(std::uint8_t goal_seq, std::uint8_t rel_seq)
  {
    act_out_payload_ = {kActCancel, rel_seq, goal_seq};
    queue_frame(kActionsTopic, seq_act_, act_out_payload_, false);
    ++seq_act_;
    flush();
    act_out_active_ = true;
    act_out_rel_ = rel_seq;
    act_out_retries_ = 0;
    act_out_deadline_ms_ = now_ms() + kRelRetryMs;
    act_host_cancels_.insert(goal_seq);
  }

    // The pending CANCEL goes out before the pending GOALs: it answers a
    // live goal under its own retry. GOALs promote in index order.
  void promote_act_pending()
  {
    if (act_pending_cancel_) {
      const std::uint8_t goal_seq = act_pending_cancel_goal_;
      act_pending_cancel_ = false;
      const std::uint8_t rel_seq = rel_next_act_++;
      emit_act_cancel(goal_seq, rel_seq);
      return;
    }
    if (act_pending_goals_.empty()) {
      return;
    }
    const auto next = act_pending_goals_.begin();
    const std::uint8_t index = next->first;
    const std::uint8_t goal_seq = next->second.goal_seq;
    const std::vector<std::uint8_t> payload = next->second.payload;
    act_pending_goals_.erase(next);
    const std::uint8_t rel_seq = rel_next_act_++;
    emit_act_goal(index, goal_seq, payload, rel_seq);
  }

    // The outstanding action frame's retry ([REL-2]): the same rel_seq
    // under the next frame seq. Spent retries drop the frame, count it and
    // promote the next.
  void poll_act_reliable(std::int64_t now)
  {
    if (!act_out_active_ || now < act_out_deadline_ms_) {
      return;
    }
    if (!queue_.empty()) {
      return;
    }
    if (act_out_retries_ >= kRelRetries) {
      log(SessionLog::kWarning, "no ack for the action frame; the frame is dropped");
      ++reliable_dropped_;
      act_out_active_ = false;
        // A dropped frame ends that wire goal: a late answer names a seq
        // nothing tracks anymore and is ignored as stale.
      if (!act_out_payload_.empty()) {
        if (act_out_payload_[0] == kActGoal && act_out_payload_.size() > 2u) {
          act_host_goals_.erase(act_out_payload_[2]);
        } else if (act_out_payload_[0] == kActCancel && act_out_payload_.size() > 2u) {
          act_host_cancels_.erase(act_out_payload_[2]);
        }
      }
      promote_act_pending();
      return;
    }
    ++act_out_retries_;
    act_out_deadline_ms_ = now + kRelRetryMs;
    queue_frame(kActionsTopic, seq_act_, act_out_payload_, false);
    ++seq_act_;
    flush();
  }

    // One incoming service frame ([SVR-4] to [SVR-7], [REL-3]). Walk answers
    // arrive while handshaking and are never acked; REQUEST and RESPONSE
    // frames ride the reliable channel while connected, acked every time
    // and applied at most once.
  void on_services(const std::uint8_t *payload, std::uint16_t len)
  {
    if (len < 1u) {
      return;
    }
    switch (payload[0]) {
      case kSvcDeclare: {
          if (state != SessionState::kHandshaking || !awaiting_svc_ || len < 2u ||
            payload[1] != svc_index_)
          {
            return;
          }
          on_service_declare(payload, len);
          return;
        }
      case kSvcStatus:
        if (state == SessionState::kHandshaking && awaiting_svc_) {
          on_service_status(payload, len);
        }
        return;
      case kSvcRequest: {
          if (state != SessionState::kConnected || len < 4u) {
            return;
          }
          send_ack_frame(kServicesTopic, payload[1]);
          std::uint8_t & expected = rel_expected_[kServicesTopic];
          if (payload[1] != expected) {
            return;
          }
          ++expected;
          const std::uint8_t index = payload[3];
          const ServiceInfo * known = nullptr;
          for (const ServiceInfo & service : services_) {
            if (service.index == index && service.status == kServiceStatusOk) {
              known = &service;
              break;
            }
          }
          if (known == nullptr) {
            log(SessionLog::kWarning, "SVC REQUEST for an unserved index is dropped");
            return;
          }
          if (svc_mcu_req_active_) {
            log(SessionLog::kWarning, "a second SVC REQUEST is dropped; the first is answered");
            return;
          }
          svc_mcu_req_active_ = true;
          svc_mcu_req_ = payload[2];
          svc_mcu_index_ = index;
          if (events.service_request) {
            events.service_request(index, payload[2],
              std::vector<std::uint8_t>(payload + 4u, payload + len));
          }
          return;
        }
      case kSvcResponse: {
          if (state != SessionState::kConnected || len < 4u || payload[3] > kSvcBusy) {
            return;
          }
          send_ack_frame(kServicesTopic, payload[1]);
          std::uint8_t & expected = rel_expected_[kServicesTopic];
          if (payload[1] != expected) {
            return;
          }
          ++expected;
          if (!svc_host_req_active_ || payload[2] != svc_host_req_) {
            return;
          }
          svc_host_req_active_ = false;
          if (events.service_response) {
            events.service_response(payload[2], payload[3],
              std::vector<std::uint8_t>(payload + 4u, payload + len));
          }
          return;
        }
      default:
        break;
    }
  }

    // One incoming action frame ([ACT-1], [ACT-3] to [ACT-8]). Walk answers
    // arrive while handshaking and are never acked; GOAL_RESP, CANCEL_RESP
    // and RESULT ride the reliable channel while connected, acked every
    // time and applied at most once. FEEDBACK is best effort: never acked.
  void on_actions(const std::uint8_t *payload, std::uint16_t len)
  {
    if (len < 1u) {
      return;
    }
    switch (payload[0]) {
      case kActDeclare: {
          if (state != SessionState::kHandshaking || !awaiting_act_ || len < 2u ||
            payload[1] != act_index_)
          {
            return;
          }
          on_action_declare(payload, len);
          return;
        }
      case kActStatus:
        if (state == SessionState::kHandshaking && awaiting_act_) {
          on_action_status(payload, len);
        }
        return;
      case kActGoalResp: {
          if (state != SessionState::kConnected || len != 4u || payload[3] > kActUnknownAction) {
            return;
          }
          send_ack_frame(kActionsTopic, payload[1]);
          std::uint8_t & expected = rel_expected_[kActionsTopic];
          if (payload[1] != expected) {
            return;
          }
          ++expected;
          if (act_host_goals_.count(payload[2]) == 0u) {
            return;
          }
          act_host_goals_.erase(payload[2]);
          if (events.action_goal_response) {
            events.action_goal_response(payload[2], payload[3]);
          }
          return;
        }
      case kActCancelResp: {
          if (state != SessionState::kConnected || len != 4u || payload[3] > kActUnknownGoal) {
            return;
          }
          send_ack_frame(kActionsTopic, payload[1]);
          std::uint8_t & expected = rel_expected_[kActionsTopic];
          if (payload[1] != expected) {
            return;
          }
          ++expected;
          if (act_host_cancels_.count(payload[2]) == 0u) {
            return;
          }
          act_host_cancels_.erase(payload[2]);
          if (events.action_cancel_response) {
            events.action_cancel_response(payload[2], payload[3]);
          }
          return;
        }
      case kActFeedback: {
          if (state != SessionState::kConnected || len < 3u) {
            return;
          }
          const std::uint8_t goal_seq = payload[1];
          const std::uint8_t index = payload[2];
          const ActionInfo * known = nullptr;
          for (const ActionInfo & action : actions_) {
            if (action.index == index && action.status == kActionStatusOk) {
              known = &action;
              break;
            }
          }
          if (known == nullptr) {
            log(SessionLog::kWarning, "ACT FEEDBACK for an unserved index is dropped");
            return;
          }
          if (events.action_feedback) {
            events.action_feedback(goal_seq, index,
              std::vector<std::uint8_t>(payload + 3u, payload + len));
          }
          return;
        }
      case kActResult: {
          if (state != SessionState::kConnected || len < 4u || payload[3] > kActAborted ||
            payload[3] < kActSucceeded)
          {
            return;
          }
          send_ack_frame(kActionsTopic, payload[1]);
          std::uint8_t & expected = rel_expected_[kActionsTopic];
          if (payload[1] != expected) {
            return;
          }
          ++expected;
          if (events.action_result) {
            events.action_result(payload[2], payload[3],
              std::vector<std::uint8_t>(payload + 4u, payload + len));
          }
          return;
        }
      default:
        break;
    }
  }

    // An ack for the outstanding SET clears it and promotes the oldest
    // pending one; anything else is a duplicate or a stranger ([REL-2]).
  void on_ack_frame(const std::uint8_t *payload, std::uint16_t len)
  {
    if (state != SessionState::kConnected || len != 2u) {
      return;
    }
    if (payload[0] == kParamsTopic) {
      if (!rel_out_active_ || payload[1] != rel_out_rel_) {
        return;
      }
      rel_out_active_ = false;
      promote_pending();
      return;
    }
    if (payload[0] == kServicesTopic) {
      if (!svc_out_active_ || payload[1] != svc_out_rel_) {
        return;
      }
      svc_out_active_ = false;
      promote_svc_pending();
    }
    if (payload[0] == kActionsTopic) {
      if (!act_out_active_ || payload[1] != act_out_rel_) {
        return;
      }
      act_out_active_ = false;
      promote_act_pending();
    }
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
      !reader.at_end())
    {
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
    for (const TopicInfo & info : topics) {
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

  void on_frame(const Frame & frame)
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
    if (topic_id == kServicesTopic) {
      on_services(payload, len);
      return;
    }
    if (topic_id == kActionsTopic) {
      on_actions(payload, len);
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
    for (const TopicInfo & topic : topics) {
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
        } else if (awaiting_svc_) {
          if (now >= svc_deadline_ms_) {
            if (svc_retries_ >= kSvcRetries) {
              log(SessionLog::kWarning, "no SVC_DECLARE answered; the handshake starts over");
              restart_handshake();
              send_hello_req();
            } else {
              ++svc_retries_;
              send_svc_query();
            }
          }
        } else if (awaiting_act_) {
          if (now >= act_deadline_ms_) {
            if (act_retries_ >= kActRetries) {
              log(SessionLog::kWarning, "no ACT_DECLARE answered; the handshake starts over");
              restart_handshake();
              send_hello_req();
            } else {
              ++act_retries_;
              send_act_query();
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
          poll_svc_reliable(now);
          poll_act_reliable(now);
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
  const Clock & clock;
  const Clock & ros_clock;
  TransportFactory open;
  TopicResolver resolve;
  SessionEvents events;
  ServiceResolver resolve_service;
  ActionResolver resolve_action;

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
    // The services walk ([SVR-7]): whether the parameter walk handed over,
    // the index under request, its retries and its deadline.
  bool awaiting_svc_ = false;
  std::uint8_t svc_index_ = 0;
  int svc_retries_ = 0;
  std::int64_t svc_deadline_ms_ = 0;
    // The services reliable stream ([REL-2], [SVR-4], [SVR-10]): one
    // outstanding REQUEST or RESPONSE with its timer and retries, one
    // pending REQUEST per index, one pending RESPONSE, and the next rel_seq
    // of the channel's own stream. The MCU's outstanding call travels as
    // its req_id and index, and the host's outstanding REQUEST as its req_id.
  bool svc_out_active_ = false;
  std::uint8_t svc_out_rel_ = 0;
  std::vector<std::uint8_t> svc_out_payload_;
  int svc_out_retries_ = 0;
  std::int64_t svc_out_deadline_ms_ = 0;
  struct SvcPendingRequest
  {
    std::uint8_t req = 0;
    std::vector<std::uint8_t> payload;
  };
  std::map<std::uint8_t, SvcPendingRequest> svc_pending_requests_;
  bool svc_pending_response_ = false;
  std::uint8_t svc_pending_response_req_ = 0;
  std::uint8_t svc_pending_response_status_ = 0;
  std::vector<std::uint8_t> svc_pending_response_payload_;
  bool svc_mcu_req_active_ = false;
  std::uint8_t svc_mcu_req_ = 0;
  std::uint8_t svc_mcu_index_ = 0;
  bool svc_host_req_active_ = false;
  std::uint8_t svc_host_req_ = 0;
    // The actions walk ([ACT-7]): whether the service walk handed over,
    // the index under request, its retries and its deadline.
  bool awaiting_act_ = false;
  std::uint8_t act_index_ = 0;
  int act_retries_ = 0;
  std::int64_t act_deadline_ms_ = 0;
    // The actions reliable stream ([REL-2], [ACT-3]): one outstanding GOAL
    // or CANCEL with its timer and retries, one pending GOAL per index,
    // one pending CANCEL, and the next rel_seq of the channel's own
    // stream. The host's outstanding GOAL and CANCEL travel with their
    // goal_seq for the answer.
  bool act_out_active_ = false;
  std::uint8_t act_out_rel_ = 0;
  std::vector<std::uint8_t> act_out_payload_;
  int act_out_retries_ = 0;
  std::int64_t act_out_deadline_ms_ = 0;
  struct ActPendingGoal
  {
    std::uint8_t goal_seq = 0;
    std::vector<std::uint8_t> payload;
  };
  std::map<std::uint8_t, ActPendingGoal> act_pending_goals_;
  bool act_pending_cancel_ = false;
  std::uint8_t act_pending_cancel_goal_ = 0;
    // The host's GOALs and CANCELs awaiting the MCU's answer, by wire
    // goal_seq. Several actions may hold one each while connected, and
    // answers may arrive in any order, so one slot per kind is not enough.
  std::set<std::uint8_t> act_host_goals_;
  std::set<std::uint8_t> act_host_cancels_;
  std::uint8_t rel_next_ = 0;
  std::uint8_t rel_next_svc_ = 0;
  std::uint8_t rel_next_act_ = 0;
  std::map<std::uint8_t, std::uint8_t> rel_expected_;
  std::uint32_t reliable_dropped_ = 0;
  std::uint32_t reliable_replaced_ = 0;

  std::vector<TopicInfo> collected;
  std::vector<TopicInfo> topics;
    // The parameter walk's DECLAREs ([PAR-7]): the walk collects them and
    // READY keeps them for the parameters layer.
  std::vector<ParamInfo> collected_params;
  std::vector<ParamInfo> params;
    // The service walk's DECLAREs ([SVR-7]): the walk collects them and
    // READY keeps them for the services layer.
  std::vector<ServiceInfo> collected_services;
  std::vector<ServiceInfo> services_;
    // The action walk's DECLAREs ([ACT-7]): the walk collects them and
    // READY keeps them for the actions layer.
  std::vector<ActionInfo> collected_actions;
  std::vector<ActionInfo> actions_;
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
    // The frame seq of the services channel, restarting at each handshake
    // and at READY ([FRM-4], [SVR-4]).
  std::uint8_t seq_svc_ = 0;
    // The frame seq of the actions channel, restarting at each handshake
    // and at READY ([FRM-4], [ACT-3]).
  std::uint8_t seq_act_ = 0;
    // The seq the time channel shares between its PINGs and CORRECTIONs,
    // restarting at 0 after each handshake ([FRM-4], [FRM-5]).
  std::uint8_t seq_time = 0;
    // The next seq per host-to-MCU topic id, restarting at 0 after each
    // handshake ([FRM-4], [SES-11]).
  std::map<std::uint8_t, std::uint8_t> user_seq_;
};

Session::Session(
  SessionConfig config, const Clock & clock, const Clock & ros_clock, TransportFactory open,
  TopicResolver resolve, SessionEvents events, ServiceResolver resolve_service,
  ActionResolver resolve_action)
: impl_(std::make_unique<Impl>(
    config, clock, ros_clock, std::move(open), std::move(resolve), std::move(events),
    std::move(resolve_service), std::move(resolve_action)))
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

const std::vector<TopicInfo> & Session::topics() const
{
  return impl_->topics;
}

// The parameter walk's DECLAREs, in announcement order.
const std::vector<ParamInfo> & Session::params() const
{
  return impl_->params;
}

const std::vector<ServiceInfo> & Session::services() const
{
  return impl_->services_;
}

const std::vector<ActionInfo> & Session::actions() const
{
  return impl_->actions_;
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

bool Session::send_param_set(std::uint8_t index, const std::vector<std::uint8_t> & value)
{
  Impl & impl = *impl_;
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

bool Session::send_service_request(
  std::uint8_t index, std::uint8_t req_id, const std::vector<std::uint8_t> & payload)
{
  Impl & impl = *impl_;
  if (impl.state != SessionState::kConnected) {
    return false;
  }
  const ServiceInfo * known = nullptr;
  for (const ServiceInfo & service : impl.services_) {
    if (service.index == index) {
      known = &service;
      break;
    }
  }
  if (known == nullptr || known->status != kServiceStatusOk) {
    return false;
  }
  const std::size_t cap =
    std::max<std::uint16_t>(impl.hello.rx_capacity, impl.hello.max_frame);
  if (payload.size() + 4u > cap) {
    return false;
  }
  if (impl.pending_bytes_ > 0u && impl.pending_bytes_ >= cap) {
    return false;
  }
  if (!impl.svc_out_active_) {
    const std::uint8_t rel_seq = impl.rel_next_svc_++;
    impl.emit_svc_request(index, req_id, payload, rel_seq);
  } else {
    const auto found = impl.svc_pending_requests_.find(index);
    if (found != impl.svc_pending_requests_.end()) {
      found->second = Impl::SvcPendingRequest{req_id, payload};
      ++impl.reliable_replaced_;
    } else {
      impl.svc_pending_requests_.emplace(index, Impl::SvcPendingRequest{req_id, payload});
    }
  }
  return true;
}

bool Session::send_service_response(
  std::uint8_t req_id, std::uint8_t status, const std::vector<std::uint8_t> & payload)
{
  Impl & impl = *impl_;
  if (impl.state != SessionState::kConnected) {
    return false;
  }
  if (!impl.svc_mcu_req_active_ || req_id != impl.svc_mcu_req_) {
    return false;
  }
  const std::size_t cap =
    std::max<std::uint16_t>(impl.hello.rx_capacity, impl.hello.max_frame);
  if (payload.size() + 4u > cap) {
    return false;
  }
  if (impl.pending_bytes_ > 0u && impl.pending_bytes_ >= cap) {
    return false;
  }
  if (!impl.svc_out_active_) {
    const std::uint8_t rel_seq = impl.rel_next_svc_++;
    impl.emit_svc_response(req_id, status, payload, rel_seq);
  } else if (impl.svc_pending_response_) {
    impl.svc_pending_response_req_ = req_id;
    impl.svc_pending_response_status_ = status;
    impl.svc_pending_response_payload_ = payload;
    ++impl.reliable_replaced_;
  } else {
    impl.svc_pending_response_ = true;
    impl.svc_pending_response_req_ = req_id;
    impl.svc_pending_response_status_ = status;
    impl.svc_pending_response_payload_ = payload;
  }
  return true;
}

bool Session::send_action_goal(
  std::uint8_t index, std::uint8_t goal_seq, const std::vector<std::uint8_t> & payload)
{
  Impl & impl = *impl_;
  if (impl.state != SessionState::kConnected) {
    return false;
  }
  const ActionInfo * known = nullptr;
  for (const ActionInfo & action : impl.actions_) {
    if (action.index == index) {
      known = &action;
      break;
    }
  }
  if (known == nullptr || known->status != kActionStatusOk) {
    return false;
  }
  const std::size_t cap =
    std::max<std::uint16_t>(impl.hello.rx_capacity, impl.hello.max_frame);
  if (payload.size() + 4u > cap) {
    return false;
  }
  if (impl.pending_bytes_ > 0u && impl.pending_bytes_ >= cap) {
    return false;
  }
  if (!impl.act_out_active_) {
    const std::uint8_t rel_seq = impl.rel_next_act_++;
    impl.emit_act_goal(index, goal_seq, payload, rel_seq);
  } else {
    const auto found = impl.act_pending_goals_.find(index);
    if (found != impl.act_pending_goals_.end()) {
      found->second = Impl::ActPendingGoal{goal_seq, payload};
      ++impl.reliable_replaced_;
    } else {
      impl.act_pending_goals_.emplace(index, Impl::ActPendingGoal{goal_seq, payload});
    }
  }
  return true;
}

bool Session::send_action_cancel(std::uint8_t goal_seq)
{
  Impl & impl = *impl_;
  if (impl.state != SessionState::kConnected) {
    return false;
  }
  const std::size_t cap =
    std::max<std::uint16_t>(impl.hello.rx_capacity, impl.hello.max_frame);
  // GCOVR_EXCL_START: reaching a connected session needs its frames to fit
  // the MCU window, so a connected cap always holds a cancel.
  if (3u > cap) {
    return false;
  }
  // GCOVR_EXCL_STOP
  if (impl.pending_bytes_ > 0u && impl.pending_bytes_ >= cap) {  // GCOV_EXCL_BR_LINE: full-window drop covered by TxWindowIsFull test
    return false;
  }
  if (!impl.act_out_active_) {
    const std::uint8_t rel_seq = impl.rel_next_act_++;
    impl.emit_act_cancel(goal_seq, rel_seq);
  } else if (impl.act_pending_cancel_) {
    impl.act_pending_cancel_goal_ = goal_seq;
    ++impl.reliable_replaced_;
  } else {
    impl.act_pending_cancel_ = true;
    impl.act_pending_cancel_goal_ = goal_seq;
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

bool Session::send_user(std::uint8_t topic_id, const std::vector<std::uint8_t> & payload)
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

}  // namespace r2s_bridge
