// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The host session: the transcript vectors replayed step by step, plus the
// deadlines, retries and reset rules that a transcript cannot pin to a
// millisecond. Time is the manual clock's alone.
// covers: SES-12, SES-13, SES-14, SES-15, SES-16, SES-17, SES-18, SES-19, SES-20, SES-21, CTL-3
// covers: CTL-9, CTL-13, SES-32, SES-33
// covers: LOG-1, LOG-2, LOG-3, LOG-4, ENC-14
// covers: ERR-1, ERR-2, ERR-5, ERR-6
#include <gtest/gtest.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fake_transport.hpp"
#include "frame_helpers.hpp"
#include "manual_clock.hpp"
#include "r2s_bridge/session.hpp"

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

constexpr std::chrono::milliseconds kTick{1};
constexpr std::uint8_t kControl = 0u;
constexpr std::uint8_t kHeartbeat = 4u;

std::vector<std::uint8_t> bytes_of(const std::string & hex)
{
  std::vector<std::uint8_t> bytes;
  std::istringstream in(hex);
  std::string pair;
  while (in >> pair) {
    bytes.push_back(static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16)));
  }
  return bytes;
}

std::string hex_of(const std::vector<std::uint8_t> & bytes)
{
  std::ostringstream out;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    if (i != 0) {
      out << ' ';
    }
    out << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(bytes[i]);
  }
  return out.str();
}

// The wire form of a decoded frame: rCOBS and the delimiter, which is exactly
// what a transcript's frame bytes are.
std::vector<std::uint8_t> wire_of(const std::vector<std::uint8_t> & decoded)
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

// A control payload as the decoded frame that carries it.
std::vector<std::uint8_t> with_topic_frame(const std::vector<std::uint8_t> & payload)
{
  std::vector<std::uint8_t> frame{kControl, 0u};
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

// A decoded frame without its check, now with it ([CHK-3]).
std::vector<std::uint8_t> with_check(std::vector<std::uint8_t> frame)
{
  const std::uint16_t crc =
    r2s_crc16_update_buf(R2S_CRC16_INIT, frame.data(), static_cast<std::uint16_t>(frame.size()));
  frame.push_back(static_cast<std::uint8_t>(crc & 0xFFu));
  frame.push_back(static_cast<std::uint8_t>(crc >> 8u));
  return frame;
}
// The wire form of a time-channel frame carrying the payload.
std::vector<std::uint8_t> time_wire(std::uint8_t seq, const std::vector<std::uint8_t> & payload)
{
  std::vector<std::uint8_t> decoded{0x01u, seq};
  decoded.insert(decoded.end(), payload.begin(), payload.end());
  return wire_of(with_check(decoded));
}

// The decoded frame behind one wire unit, checked: rCOBS without the
// delimiter, exactly what the session wrote.
std::vector<std::uint8_t> decoded_of(std::vector<std::uint8_t> unit)
{
  if (unit.empty() || unit.back() != 0x00u) {
    throw std::invalid_argument("decoded_of needs one delimited unit");
  }
  unit.pop_back();
  uint8_t *decoded = nullptr;
  uint16_t decoded_len = 0;
  if (r2s_rcobs_decode(unit.data(), static_cast<std::uint16_t>(unit.size()), &decoded,
          &decoded_len) != 0)
  {
    throw std::invalid_argument("decoded_of refuses a bad encoding");
  }
  return std::vector<std::uint8_t>(decoded, decoded + decoded_len);
}

std::vector<std::vector<std::uint8_t>> units_of(const std::vector<std::uint8_t> & bytes)
{
  std::vector<std::vector<std::uint8_t>> units;
  std::vector<std::uint8_t> current;
  for (const std::uint8_t byte : bytes) {
    current.push_back(byte);
    if (byte == 0x00u) {
      units.push_back(current);
      current.clear();
    }
  }
  if (!current.empty()) {
    units.push_back(current);
  }
  return units;
}

// One HELLO payload: the fields of [CTL-16], with the defaults a compliant
// MCU announces. The token is what the MCU echoes from RAM: 0 before the
// host assigns one.
std::vector<std::uint8_t> hello_payload(
  std::uint32_t token, std::uint8_t topics, std::uint8_t version = 4u,
  std::uint8_t features = 0u, std::uint32_t tick_hz = 1000000u)
{
  std::vector<std::uint8_t> p{0x02u, version, 0x00u};
  const auto put_le = [&p](std::uint32_t value, int bytes) {
      for (int i = 0; i < bytes; ++i) {
        p.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
      }
    };
  put_le(token, 4);
  put_le(512, 2);   // max_frame
  put_le(515, 2);   // rx_capacity
  put_le(tick_hz, 4);
  p.push_back(features);
  p.push_back(topics);
  const std::string name = "node";
  p.push_back(static_cast<std::uint8_t>(name.size()));
  p.insert(p.end(), name.begin(), name.end());
  return p;
}

std::vector<std::uint8_t> topic_payload(
  std::uint8_t index, std::uint8_t topic_id, const std::string & name,
  std::uint8_t flags = 0x00u)
{
  std::vector<std::uint8_t> p{0x04u, index, topic_id, flags};
  for (std::uint8_t i = 0; i < 8u; ++i) {
    p.push_back(i);
  }
  const std::string type = "std_msgs/msg/Float32";
  p.push_back(static_cast<std::uint8_t>(type.size()));
  p.insert(p.end(), type.begin(), type.end());
  p.push_back(static_cast<std::uint8_t>(name.size()));
  p.insert(p.end(), name.begin(), name.end());
  return p;
}

// The session opens the test's own fake, so a reopen keeps its script and
// capture. It never closes that fake.
class Link final : public Transport
{
public:
  explicit Link(FakeTransport & inner)
  : inner_(inner)
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
  FakeTransport & inner_;
};

// A session over the test's own fake link, driven by the test's clocks only.
// Steady and ROS time advance together: the mapping test that separates them
// is the estimator's own, in test_time_sync.cpp.
class Fixture
{
public:
  explicit Fixture(
    TopicResolver resolve = [] (const TopicInfo &) {return std::uint8_t{0};},
    SessionConfig config = SessionConfig(), SessionEvents events = SessionEvents(),
    ServiceResolver service_resolve = ServiceResolver(),
    ActionResolver action_resolve = ActionResolver())
  {
    session_ = std::make_unique<Session>(
      config,
      clock_,
      ros_clock_,
      [this]() {
        return std::unique_ptr<Transport>(new Link(transport_));
      },
      std::move(resolve),
      std::move(events),
      std::move(service_resolve),
      std::move(action_resolve));
  }

    // Moves time one tick at a time, polling after each, so a timer fires on
    // the first tick at which its deadline has passed.
  void advance_to(std::uint32_t ms)
  {
    while (now_ms_ < ms) {
      clock_.advance(kTick);
      ros_clock_.advance(kTick);
      ++now_ms_;
      session_->poll();
    }
  }

  void deliver(const std::vector<std::uint8_t> & decoded)
  {
    transport_.script_read(wire_of(decoded));
    session_->poll();
  }

    // Delivers one decoded frame: the caller passes topic, seq and payload,
    // and the check is appended here, as [CHK-3] lays it out.
  void deliver_frame(
    std::uint8_t topic_id, std::uint8_t seq,
    const std::vector<std::uint8_t> & payload)
  {
    std::vector<std::uint8_t> decoded{topic_id, seq};
    decoded.insert(decoded.end(), payload.begin(), payload.end());
    const std::uint16_t crc =
      r2s_crc16_update_buf(R2S_CRC16_INIT, decoded.data(),
            static_cast<std::uint16_t>(decoded.size()));
    decoded.push_back(static_cast<std::uint8_t>(crc & 0xFFu));
    decoded.push_back(static_cast<std::uint8_t>(crc >> 8u));
    deliver(decoded);
  }

    // The host frames written since the last drain.
  std::vector<std::vector<std::uint8_t>> drain_units()
  {
    std::vector<std::uint8_t> all;
    for (const auto & write : transport_.writes()) {
      all.insert(all.end(), write.begin(), write.end());
    }
    transport_.discard_writes();
    return units_of(all);
  }

    // Caps the link so every write blocks, or opens it again. Pending frames
    // stay queued while blocked, which is how the tests wedge the link.
  void block_writes()
  {
    transport_.limit_write(0u);
  }

  void allow_writes()
  {
    transport_.limit_write(1000000u);
  }

    // Queues one scripted write outcome on the fake link.
  void script_write(IoResult outcome)
  {
    transport_.script_write(outcome);
  }

  Session & session()
  {
    return *session_;
  }
  std::uint32_t now_ms() const
  {
    return now_ms_;
  }

private:
  ManualClock clock_;
  ManualClock ros_clock_;
  FakeTransport transport_;
  std::uint32_t now_ms_ = 0;
  std::unique_ptr<Session> session_;
};

struct Step
{
  bool from_host = false;
  std::vector<std::uint8_t> decoded;
  std::uint32_t at_ms = 0;
};

// Transcripts with their own replay below: the walk pair carries no PING
// steps for the time sync the frozen vectors predate, so it replays with
// time sync off; the reliable trio starts mid-connection and filters to
// the parameter and ack channels. The service and action transcripts replay
// in X6 (services) and X10c/X10d (actions). The stock replay covers the rest.
std::vector<std::string> transcript_vectors()
{
  static const std::set<std::string> own_replay = {"param_walk.yaml",
    "param_walk_retry.yaml",
    "param_set_reliable.yaml",
    "param_set_giveup.yaml",
    "param_rehandshake.yaml",
    "service_walk.yaml",
    "service_not_connected.yaml",
    "service_call.yaml",
    "action_walk.yaml",
    "action_goal.yaml",
    "action_cancel.yaml",
    "action_second_goal.yaml",
    "action_reset_abort.yaml"};
  std::vector<std::string> files;
  for (const auto & entry : std::filesystem::directory_iterator(R2S_SESSION_VECTOR_DIR)) {
    if (entry.path().extension() != ".yaml") {
      continue;
    }
    if (own_replay.count(entry.path().filename().string()) != 0u) {
      continue;
    }
    files.push_back(entry.path().string());
  }
  std::sort(files.begin(), files.end());
  return files;
}

class SessionTranscript : public ::testing::TestWithParam<std::string>
{
};

// Replays one transcript through the host session: every MCU step arrives at
// its own time, and the host answers with exactly the transcript's frames.
// Parses one transcript file into its name and steps.
std::pair<std::string, std::vector<Step>> load_steps(const std::string & path)
{
  const YAML::Node root = YAML::LoadFile(path);
  const std::string name = root["name"].as<std::string>();
  std::vector<Step> steps;
    // A step without its own time runs with the previous one: the walk
    // vectors leave it off the same-tick answers.
  std::uint32_t at_ms = 0;
  for (const auto & node : root["steps"]) {
    Step step;
    step.from_host = node["from"].as<std::string>() == "host";
    step.decoded = bytes_of(node["frame"].as<std::string>());
    if (node["at_ms"]) {
      at_ms = node["at_ms"].as<std::uint32_t>();
    }
    step.at_ms = at_ms;
    steps.push_back(std::move(step));
  }
  return {name, steps};
}

void replay_transcript(
  const std::string & path, SessionConfig config = SessionConfig(),
  ServiceResolver service_resolve = ServiceResolver(),
  ActionResolver action_resolve = ActionResolver())
{
  ASSERT_TRUE(std::filesystem::exists(path)) << path << ": the transcript is missing";
  const auto [name, steps] = load_steps(path);
  ASSERT_FALSE(steps.empty()) << name;

  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    std::move(service_resolve), std::move(action_resolve)};
  std::vector<std::size_t> expected;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    if (steps[i].from_host) {
      expected.push_back(i);
    }
  }

  std::size_t next = 0;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const Step & step = steps[i];
    SCOPED_TRACE(name + " step " + std::to_string(i));
    fixture.advance_to(step.at_ms);
    if (!step.from_host) {
      fixture.deliver(step.decoded);
    }
    const std::vector<std::vector<std::uint8_t>> written = fixture.drain_units();
    for (const auto & unit : written) {
      ASSERT_LT(next, expected.size()) << name << ": the session wrote more than the transcript";
      const Step & want = steps[expected[next]];
            // The capture is the wire form, and so is a transcript step's bytes.
      EXPECT_EQ(hex_of(unit), hex_of(wire_of(want.decoded)))
                << name << " host step " << expected[next] << " at " << fixture.now_ms() << " ms";
      ++next;
    }
  }

    // A transcript may end on a host frame the session has not sent yet: the
    // timer is due, so give it the tick it waits for.
  if (next < expected.size()) {
    fixture.advance_to(steps.back().at_ms + 500);
    for (const auto & unit : fixture.drain_units()) {
      ASSERT_LT(next, expected.size()) << name << ": the session wrote more than the transcript";
      EXPECT_EQ(hex_of(unit), hex_of(wire_of(steps[expected[next]].decoded)))
                << name << " host step " << expected[next];
      ++next;
    }
  }
  EXPECT_EQ(next, expected.size()) << name << ": the transcript's host frames did not all go out";
}

TEST_P(SessionTranscript, ReplaysTheTranscriptInOrder)
{
    replay_transcript(GetParam());
}

TEST(SessionState, TimeIsUnavailableBeforeTheHandshake)
{
    Fixture fixture;
    EXPECT_FALSE(fixture.session().time_available());
}

TEST(SessionState, McuLimitsAreZeroBeforeTheHello)
{
    Fixture fixture;
    EXPECT_EQ(fixture.session().mcu_max_frame(), 0u);
    EXPECT_EQ(fixture.session().mcu_rx_capacity(), 0u);
}

TEST(SessionConfig, RejectsAPeriodThatIsNotPositive)
{
    const auto resolve = [](const TopicInfo &) {return std::uint8_t{0};};
    SessionConfig reconnect = SessionConfig();
    reconnect.reconnect_period_ms = 0;
    EXPECT_THROW(Fixture(resolve, reconnect), std::invalid_argument);
    SessionConfig boot = SessionConfig();
    boot.boot_wait_ms = -1;
    EXPECT_THROW(Fixture(resolve, boot), std::invalid_argument);
    SessionConfig heartbeat = SessionConfig();
    heartbeat.heartbeat_period_ms = 0;
    EXPECT_THROW(Fixture(resolve, heartbeat), std::invalid_argument);
}

TEST(SessionHandshake, RejectsATickRateBelowOneKilohertz)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 4u, 0u, 500u));
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().seen_hello());
}

TEST(SessionHandshake, RefusesAHelloWithOnlyAnOpcode)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, std::vector<std::uint8_t>{0x02u});
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().seen_hello());
}

TEST(SessionHandshake, RefusesAHelloCutAfterTheVersion)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    hello.resize(2u);
    fixture.deliver_frame(kControl, 0u, hello);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().seen_hello());
}

TEST(SessionHandshake, RefusesAHelloCutAfterTheMaxFrame)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    hello.resize(8u);
    fixture.deliver_frame(kControl, 0u, hello);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().seen_hello());
}

TEST(SessionHandshake, RefusesAHelloCutAfterTheFeatures)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    hello.resize(16u);
    fixture.deliver_frame(kControl, 0u, hello);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().seen_hello());
}

TEST(SessionHandshake, StartsOverOnAHelloWithANewTokenMidWalk)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "the TOPIC_REQ of the walk";
    fixture.deliver_frame(kControl, 0u, hello_payload(0x2000u, 1u));
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(units[0][2], 0x03u) << "the TOPIC_REQ of the new walk";
}

TEST(SessionHandshake, RefusesATopicWithANameLongerThanThePayload)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "the TOPIC_REQ of the walk";
    std::vector<std::uint8_t> topic = topic_payload(0u, 0x10u, "battery");
    topic[33] = 100u;
    fixture.deliver_frame(kControl, 0u, topic);
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
    EXPECT_TRUE(fixture.drain_units().empty()) << "no TOPIC_STATUS for a short TOPIC";
}

TEST(SessionHandshake, AcceptsATopicWithAMultiByteNameLength)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "the TOPIC_REQ of the walk";
    std::vector<std::uint8_t> topic = topic_payload(0u, 0x10u, "battery");
    topic.resize(33u);
    topic.push_back(0x82u);
    topic.push_back(0x01u);
    topic.insert(topic.end(), 130u, 'c');
    fixture.deliver_frame(kControl, 0u, topic);
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 2u);
    EXPECT_EQ(units[0][2], 0x05u) << "the TOPIC_STATUS of the accepted topic";
}

TEST(SessionHandshake, RefusesANameLengthThatNeverEnds)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "the TOPIC_REQ of the walk";
    std::vector<std::uint8_t> topic = topic_payload(0u, 0x10u, "battery");
    topic[33] = 0x80u;
    topic.insert(topic.begin() + 34, {0x80u, 0x80u, 0x00u});
    fixture.deliver_frame(kControl, 0u, topic);
    EXPECT_TRUE(fixture.drain_units().empty()) << "no TOPIC_STATUS for a bad length";
}

TEST(SessionHandshake, AcceptsTopicsWithoutAResolver)
{
    Fixture fixture(nullptr);
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u));
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

TEST(SessionConnected, CallsTheUserFrameHandler)
{
    SessionEvents events;
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> received;
    events.user_frame = [&](std::uint8_t id, const std::vector<std::uint8_t> & payload) {
      received.emplace_back(id, payload);
    };
    Fixture fixture([](const TopicInfo &) {return std::uint8_t{0};}, SessionConfig(), events);
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u));
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    fixture.deliver_frame(0x10u, 0u, std::vector<std::uint8_t>{0x01u, 0x02u});
    ASSERT_EQ(received.size(), 1u);
    EXPECT_EQ(received[0].first, 0x10u);
    EXPECT_EQ(received[0].second, (std::vector<std::uint8_t>{0x01u, 0x02u}));
}

TEST(SessionHandshake, DropsWedgedControlFramesOnRestart)
{
    Fixture fixture;
    fixture.block_writes();
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    fixture.advance_to(900);
    fixture.allow_writes();
    fixture.advance_to(901);
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u) << "the wedged TOPIC_REQ is gone, only the HELLO_REQ goes out";
    EXPECT_EQ(units[0][2], 0x01u) << "a HELLO_REQ";
    EXPECT_EQ(fixture.session().control_coalesced(), 3u);
}

TEST(SessionLink, CoalescesControlFramesOnAWedgedLink)
{
    Fixture fixture;
    fixture.block_writes();
    fixture.advance_to(3600);
    fixture.allow_writes();
    fixture.advance_to(3601);
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u) << "one HELLO_REQ, however many were queued";
    EXPECT_EQ(units[0][2], 0x01u) << "a HELLO_REQ";
    EXPECT_EQ(fixture.session().control_coalesced(), 2u);
}

// The transcripts no conforming firmware produces: their refused HELLOs come
// from the transcript, never from the MCU. The glob below replays them again;
// this repeats the two by name, so deleting one fails here instead of
// silently shrinking the glob.
TEST(SessionTranscripts, ReplaysTheRefusedHelloVectors)
{
    replay_transcript(std::string(R2S_SESSION_VECTOR_DIR) + "/refused_hello.yaml");
    replay_transcript(std::string(R2S_SESSION_VECTOR_DIR) + "/version_mismatch.yaml");
}

INSTANTIATE_TEST_SUITE_P(Transcripts, SessionTranscript, ::testing::ValuesIn(transcript_vectors()));

TEST(SessionBootWait, SendsNothingBeforeBootWaitExpires)
{
    Fixture fixture;
    fixture.advance_to(2499);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_TRUE(fixture.drain_units().empty()) << "the host must wait for the boot to finish";
}

TEST(SessionBootWait, AsksForHelloWhenBootWaitExpires)
{
    Fixture fixture;
    fixture.advance_to(2500);
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u);
}

TEST(SessionBootWait, LeavesBootWaitOnAnUnsolicitedHello)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 0u));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_EQ(fixture.session().token(), 1u) << "the completed handshake assigns token 1";
}

TEST(SessionHandshake, RepeatsHelloReqEvery500msUntilAHelloArrives)
{
    Fixture fixture;
    fixture.advance_to(2500);
    EXPECT_EQ(fixture.drain_units().size(), 1u);
    fixture.advance_to(2999);
    EXPECT_TRUE(fixture.drain_units().empty()) << "no second request before 500 ms";
    fixture.advance_to(3000);
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "a request every 500 ms";
}

TEST(SessionHandshake, RetriesTopicReqAfter200msUpToThreeTimes)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "the first TOPIC_REQ";
    for (int retry = 1; retry <= 3; ++retry) {
    fixture.advance_to(100 + 200u * static_cast<std::uint32_t>(retry) - 1u);
    EXPECT_TRUE(fixture.drain_units().empty()) << "retry " << retry << " came early";
    fixture.advance_to(100 + 200u * static_cast<std::uint32_t>(retry));
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "retry " << retry;
    }
    fixture.advance_to(100 + 200u * 4u);
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u) << "after the third retry the handshake starts over";
    EXPECT_EQ(units[0][2], 0x01u) << "a HELLO_REQ, not another TOPIC_REQ";
}

TEST(SessionHandshake, RefusesAHelloOfAnotherProtocolVersion)
{
    std::vector<std::string> logged;
    SessionEvents events;
    events.log = [&logged](SessionLog, const std::string & text) {logged.push_back(text);};
    ManualClock clock;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};}, events);
    const std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u, 1u);
    std::vector<std::uint8_t> frame{kControl, 0u};
    frame.insert(frame.end(), hello.begin(), hello.end());
    transport.script_read(wire_of(with_check(frame)));
    session.poll();
    EXPECT_EQ(session.state(), SessionState::kBootWait) << "a refused HELLO changes nothing";
    ASSERT_EQ(logged.size(), 1u);
    EXPECT_NE(logged[0].find("MCU protocol version is 1"), std::string::npos)
        << "the error names the MCU version";
    EXPECT_NE(logged[0].find("host protocol version is 4"), std::string::npos)
        << "the error names the host version";
}

TEST(SessionHandshake, AcceptsAnMcuWhoseDoubleIs32Bits)
{
    Fixture fixture;
    fixture.advance_to(10);
    // Bit 4 is informational, and every AVR build sets it ([CTL-2]).
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 4u, 0x10u));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// The HELLO the MCU core's own vector file announces, byte for byte
// (spec/vectors/frames/hello.yaml): the two sides agree on the wire, not just
// on this test's builder.
TEST(SessionHandshake, AcceptsTheHelloOfTheMcusOwnVector)
{
    const YAML::Node vector = YAML::LoadFile(std::string(R2S_FRAMES_VECTOR_DIR) + "/hello.yaml");
    const std::vector<std::uint8_t> payload = bytes_of(vector["payload"].as<std::string>());
    ASSERT_FALSE(payload.empty());

    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, payload);
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking)
        << "one topic to collect, so the walk has just begun";
    EXPECT_EQ(fixture.session().token(), 1u);
}

TEST(SessionHandshake, RefusesATickRateThatIsNotThousandTimesADivisor)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 4u, 0u, 1500u));
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 4u, 0u, 1000u));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected) << "1 kHz is a valid form";
}

TEST(SessionHandshake, SendsReadyAfterTheLastTopic)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 2u) << "the TOPIC_STATUS and then READY";
    EXPECT_EQ(hex_of(units[1]),
              hex_of(wire_of(with_check({kControl, 2u, 0x06u, 0x01u, 0x00u, 0x00u, 0x00u}))))
        << "READY assigns token 1";
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_EQ(fixture.session().token(), 1u);
    ASSERT_EQ(fixture.session().topics().size(), 1u);
    EXPECT_EQ(fixture.session().topics()[0].topic_name, "battery");
}

TEST(SessionHandshake, IgnoresARepeatHelloWithTheRecordedToken)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u));
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "the TOPIC_REQ of the walk";
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u));
    EXPECT_TRUE(fixture.drain_units().empty()) << "the repeat changes nothing";
}

TEST(SessionHandshake, RejectsOneTopicAndKeepsTheOthers)
{
    Fixture fixture([](const TopicInfo & info) {
      return info.topic_id == 0x10u ? kTopicStatusHashMismatch : kTopicStatusOk;
    });
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 2u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    const auto first = fixture.drain_units();
    ASSERT_EQ(first.size(), 2u) << "TOPIC_STATUS, then the next TOPIC_REQ";
    EXPECT_EQ(hex_of(first[0]), hex_of(wire_of(with_check({kControl, 1u, 0x05u, 0x10u, 0x02u}))))
        << "the rejected topic answers 2 (hash mismatch)";
    fixture.deliver_frame(kControl, 0u, topic_payload(1u, 0x11u, "charge"));
    const auto last = fixture.drain_units();
    ASSERT_EQ(last.size(), 2u) << "TOPIC_STATUS, then READY";
    EXPECT_EQ(hex_of(last[0]), hex_of(wire_of(with_check({kControl, 3u, 0x05u, 0x11u, 0x00u}))))
        << "the kept topic answers 0 (ok)";
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    ASSERT_EQ(fixture.session().topics().size(), 2u);
    EXPECT_EQ(fixture.session().topics()[0].status, kTopicStatusHashMismatch);
    EXPECT_EQ(fixture.session().topics()[1].status, kTopicStatusOk);
}

TEST(SessionHandshake, AnswersUnknownTypeWithStatus1)
{
    Fixture fixture([](const TopicInfo &) {return kTopicStatusUnknownType;});
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 2u) << "TOPIC_STATUS, then READY";
    EXPECT_EQ(hex_of(units[0]), hex_of(wire_of(with_check({kControl, 1u, 0x05u, 0x10u, 0x01u}))))
        << "the unknown type answers 1";
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    ASSERT_EQ(fixture.session().topics().size(), 1u);
    EXPECT_EQ(fixture.session().topics()[0].status, kTopicStatusUnknownType);
}

TEST(SessionHandshake, ReportsWhetherTheNewTableMatchesTheOldOne)
{
    std::vector<bool> identical;
    SessionEvents events;
    events.handshake_completed = [&identical](bool same) {identical.push_back(same);};
    ManualClock clock;
    FakeTransport transport;
    std::uint32_t now = 0;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};}, events);

    const auto advance = [&clock, &session, &now](std::uint32_t ms) {
      while (now < ms) {
        clock.advance(kTick);
        ++now;
        session.poll();
      }
    };
    // One topic, then the same one again after a reset, then a different one.
    advance(10);
    const std::vector<std::uint8_t> hello = hello_payload(0x1000u, 1u);
    std::vector<std::uint8_t> hello_frame{kControl, 0u};
    hello_frame.insert(hello_frame.end(), hello.begin(), hello.end());
    transport.script_read(wire_of(with_check(hello_frame)));
    session.poll();
    const std::vector<std::uint8_t> first = topic_payload(0u, 0x10u, "battery");
    std::vector<std::uint8_t> frame{kControl, 0u};
    frame.insert(frame.end(), first.begin(), first.end());
    transport.script_read(wire_of(with_check(frame)));
    session.poll();
    ASSERT_EQ(identical.size(), 1u);
    EXPECT_FALSE(identical[0]) << "there was no previous table to match";

    // The same table comes back after a reset: kept, not rebuilt.
    transport.script_read(wire_of(with_check(hello_frame)));
    session.poll();
    std::vector<std::uint8_t> again{kControl, 0u};
    again.insert(again.end(), first.begin(), first.end());
    transport.script_read(wire_of(with_check(again)));
    session.poll();
    ASSERT_EQ(identical.size(), 2u);
    EXPECT_TRUE(identical[1]) << "an identical table keeps its publishers and subscriptions";

    // A different name changes the table.
    const std::vector<std::uint8_t> second = topic_payload(0u, 0x10u, "charge");
    transport.script_read(wire_of(with_check(hello_frame)));
    session.poll();
    std::vector<std::uint8_t> changed{kControl, 0u};
    changed.insert(changed.end(), second.begin(), second.end());
    transport.script_read(wire_of(with_check(changed)));
    session.poll();
    ASSERT_EQ(identical.size(), 3u);
    EXPECT_FALSE(identical[2]);
}

TEST(SessionConnected, SendsAHeartbeatEvery500ms)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    fixture.drain_units();
    fixture.advance_to(599);
    EXPECT_TRUE(fixture.drain_units().empty());
    fixture.advance_to(600);
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(units[0][0], kHeartbeat);
}

TEST(SessionConnected, ReturnsToHandshakingAfterTheLinkTimeout)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.drain_units();
    fixture.advance_to(2099);
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.drain_units();
    fixture.advance_to(2100);
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
}

TEST(SessionConnected, ATopicFrameKeepsTheLinkAlive)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    for (std::uint32_t t = 1100; t <= 2000; t += 100) {
    fixture.advance_to(t);
    fixture.deliver_frame(0x10u, 0u, std::vector<std::uint8_t>{1u, 2u, 3u, 4u});
    }
    fixture.advance_to(2050);
    EXPECT_EQ(fixture.session().state(),
          SessionState::kConnected) << "user traffic is host traffic too";
}

TEST(SessionConnected, ResetsOnAHelloWithTheSameToken)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 0u));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    const auto first = fixture.drain_units();
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(hex_of(first[0]),
              hex_of(wire_of(with_check({kControl, 0u, 0x06u, 0x01u, 0x00u, 0x00u, 0x00u}))));
    fixture.deliver_frame(kControl, 0u, hello_payload(1u, 0u));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected)
        << "an empty table resolves at once, so the re-handshake is already done";
    const auto second = fixture.drain_units();
    ASSERT_EQ(second.size(), 1u) << "READY for the new handshake";
    EXPECT_EQ(hex_of(second[0]),
              hex_of(wire_of(with_check({kControl, 0u, 0x06u, 0x02u, 0x00u, 0x00u, 0x00u}))))
        << "the new handshake assigns a fresh token";
    EXPECT_EQ(fixture.session().token(), 2u);
}

TEST(SessionConnected, ResetsOnAHelloWithNoToken)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 0u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u));
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
    EXPECT_EQ(fixture.session().token(), 0u);
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "the TOPIC_REQ of the new table";
}

TEST(SessionConnected, RecordsTheMcuCountersFromAHeartbeat)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    fixture.deliver_frame(kHeartbeat, 0u,
          std::vector<std::uint8_t>{1u, 0u, 2u, 0u, 3u, 0u, 4u, 0u});
    const McuCounters counters = fixture.session().mcu_counters();
    EXPECT_EQ(counters.rx_frames, 1u);
    EXPECT_EQ(counters.crc_errors, 2u);
    EXPECT_EQ(counters.rx_overflows, 3u);
    EXPECT_EQ(counters.dropped_frames, 4u);
}

TEST(SessionHandshake, StartsTheHandshakeOverWhenTheRetriesAreSpent)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    for (std::uint32_t t = 100; t < 900; t += 100) {
    fixture.advance_to(t);
    }
    fixture.advance_to(900);
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
    fixture.drain_units();
    // The walk is abandoned, so the cadence is HELLO_REQ's again: no TOPIC_REQ
    // in between, and the request lands 500 ms later.
    for (std::uint32_t t = 901; t < 1400; ++t) {
    fixture.advance_to(t);
    EXPECT_TRUE(fixture.drain_units().empty()) <<
      "nothing but HELLO_REQ at 500 ms, got a frame at " << t;
    }
    fixture.advance_to(1400);
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u) << "one HELLO_REQ, and no TOPIC_REQ";
    EXPECT_EQ(units[0][2], 0x01u) << "a HELLO_REQ";
}

TEST(SessionHandshake, AcceptsTheHelloThatAnswersTheRestart)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    fixture.advance_to(900);
    fixture.drain_units();
    // One tick past the restart, the abandoned walk would fire its stale
    // deadline; only a disarmed walk stays silent here.
    fixture.advance_to(901);
    EXPECT_TRUE(fixture.drain_units().empty()) <<
          "the abandoned walk sends nothing after the restart";
    // The restart's HELLO_REQ is answered, so the walk begins again from it.
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(units[0][2], 0x03u) << "the TOPIC_REQ of the new walk";
}

TEST(SessionLink, DoesNotResumeAWalkOnAReopenedLink)
{
    ManualClock clock;
    std::uint32_t now = 0;
    int opens = 0;
    // The faulted link latches closed, so a reopen is a second fake port.
    FakeTransport first;
    FakeTransport second;
    std::unique_ptr<Session> session;
    session = std::make_unique<Session>(
        SessionConfig{}, clock, clock,
    [&opens, &first, &second]() -> std::unique_ptr<Transport> {
      ++opens;
      return std::unique_ptr<Transport>(new Link(opens == 1 ? first : second));
        },
    [](const TopicInfo &) {return std::uint8_t{0};});
    const auto advance = [&clock, &session, &now](std::uint32_t ms) {
      while (now < ms) {
        clock.advance(kTick);
        ++now;
        session->poll();
      }
    };
    advance(100);
    first.script_read(wire_of(with_check(with_topic_frame(hello_payload(0x1000u, 2u)))));
    session->poll();
    first.script_read(IoStatus::kClosed);
    session->poll();
    ASSERT_EQ(session->state(), SessionState::kDisconnected);
    // The second open waits its boot wait out in silence. Only HELLO_REQ may
    // go out: the dead link's walk must not resume here ([SES-13], [SES-14]).
    advance(3700);
    EXPECT_EQ(session->state(), SessionState::kHandshaking);
    const std::vector<std::vector<std::uint8_t>> units = units_of([&second]() {
        std::vector<std::uint8_t> all;
        for (const auto & write : second.writes()) {
          all.insert(all.end(), write.begin(), write.end());
        }
        return all;
    }());
    ASSERT_EQ(units.size(), 1u) << "one HELLO_REQ, and no TOPIC_REQ";
    EXPECT_EQ(units[0][2], 0x01u);
}

TEST(SessionConnected, UsesTheConfiguredHeartbeatPeriod)
{
    SessionConfig config;
    config.heartbeat_period_ms = 100;
    Fixture fixture([](const TopicInfo &) {return std::uint8_t{0};}, config);
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    fixture.drain_units();
    fixture.advance_to(109);
    EXPECT_TRUE(fixture.drain_units().empty());
    fixture.advance_to(110);
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "the period comes from the config";
}

TEST(SessionHandshake, RefusesAMaxFrameAboveTheProtocolLimit)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    hello[7] = 0xFEu;  // max_frame low byte: 65278, one above 65277
    hello[8] = 0xFEu;
    fixture.deliver_frame(kControl, 0u, hello);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().seen_hello());
}

TEST(SessionHandshake, RefusesACompactHandshake)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    hello[2] = 0x01u;  // the handshake mode byte ([CTL-1])
    fixture.deliver_frame(kControl, 0u, hello);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
}

TEST(SessionHandshake, RefusesATruncatedHello)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    hello.resize(10u);
    fixture.deliver_frame(kControl, 0u, hello);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().seen_hello());
}

TEST(SessionHandshake, RefusesANodeNameWithBytesLeftOver)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    hello.push_back(0x00u);  // a byte after the name
    fixture.deliver_frame(kControl, 0u, hello);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
}

TEST(SessionHandshake, RefusesAVarintThatIsNotTheShortestForm)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    // The name length is one byte at the end of the fixed fields; write it as
    // the two-byte form of a value below 128 ([VAR-4]).
    hello[17] = 0x84u;
    hello.insert(hello.begin() + 18, 0x00u);
    fixture.deliver_frame(kControl, 0u, hello);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
}

TEST(SessionLink, ReopensThePortWhenItIsNotThereYet)
{
    ManualClock clock;
    int opens = 0;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&opens, &transport]() -> std::unique_ptr<Transport> {
      ++opens;
      if (opens == 1) {
        return nullptr;
      }
      return std::unique_ptr<Transport>(new Link(transport));
    },
    [](const TopicInfo &) {return std::uint8_t{0};});
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
    EXPECT_EQ(opens, 1);
    clock.advance(std::chrono::milliseconds(999));
    session.poll();
    EXPECT_EQ(opens, 1);
    clock.advance(std::chrono::milliseconds(1));
    session.poll();
    EXPECT_EQ(opens, 2) << "a retry every reconnect_period_ms";
    EXPECT_EQ(session.state(), SessionState::kBootWait);
}

// A write that fails mid-frame is the same hangup as a failed read, and the
// receive loop must not keep reading through the reader it just closed.
TEST(SessionLink, ReturnsToDisconnectedOnAWriteError)
{
    ManualClock clock;
    std::uint32_t now = 0;
    FakeTransport incoming;
    // Reads reach the fake, writes fail: the port died while the host was
    // answering the HELLO with its TOPIC_REQ.
    class FailingWrites final : public Transport
  {
public:
    explicit FailingWrites(FakeTransport & inner)
    : inner_(inner)
    {
    }
    IoResult read(std::uint8_t *buffer, std::size_t capacity) override
    {
      return inner_.read(buffer, capacity);
    }
    IoResult write(const std::uint8_t *, std::size_t) override
    {
      return {0u, IoStatus::kError};
    }
    void close() override
    {
    }

private:
    FakeTransport & inner_;
    };
    Session session(
    SessionConfig{}, clock, clock,
    [&incoming]() {return std::unique_ptr<Transport>(new FailingWrites(incoming));},
    [](const TopicInfo &) {return std::uint8_t{0};});
    while (now < 100) {
    clock.advance(kTick);
    ++now;
    session.poll();
    }
    incoming.script_read(wire_of(with_check(with_topic_frame(hello_payload(0x1000u, 1u)))));
    // A second frame waits behind the one being answered: the loop must stop
    // at the close, not read it through the reader the close dropped.
    incoming.script_read(wire_of(with_check(with_topic_frame(hello_payload(0x2000u, 0u)))));
    session.poll();
    EXPECT_EQ(session.state(), SessionState::kDisconnected)
        << "reading after the close would hand the dead link's frame to the session";
}

TEST(SessionLink, ReturnsToDisconnectedOnAPortError)
{
    ManualClock clock;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};});
    clock.advance(std::chrono::milliseconds(100));
    session.poll();
    transport.script_read(IoStatus::kError);
    session.poll();
    EXPECT_EQ(session.state(), SessionState::kDisconnected);
}

TEST(SessionLink, RefusesANonPositivePeriod)
{
    ManualClock clock;
    SessionConfig config;
    config.link_timeout_ms = 0;
    EXPECT_THROW(Session(
                     config, clock, clock, []() {return std::unique_ptr<Transport>();},
      [](const TopicInfo &) {return std::uint8_t{0};}),
                 std::invalid_argument);
}
// A decoded wire unit back to its frame, through the stateless decoder.
std::vector<std::uint8_t> decoded_unit(std::vector<std::uint8_t> unit)
{
  unit.pop_back();
  std::uint8_t *at = nullptr;
  std::uint16_t len = 0;
  if (r2s_rcobs_decode(unit.data(), static_cast<std::uint16_t>(unit.size()), &at,
          &len) != R2S_RCOBS_OK)
  {
    return {};
  }
  return {at, at + len};
}

TEST(SessionTopics, DuplicateNameInOneDirectionIsAConflict)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 2u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 16u, "dup"));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 2u, topic_payload(1u, 17u, "dup"));
    bool refused = false;
    for (const auto & unit : fixture.drain_units()) {
    const std::vector<std::uint8_t> decoded = decoded_unit(unit);
    if (decoded.size() >= 5u && decoded[0] == kControl && decoded[2] == 0x05u &&
      decoded[3] == 17u &&
      decoded[4] == kTopicStatusNameConflict)
    {
      refused = true;
    }
    }
    EXPECT_TRUE(refused) << "the second TOPIC_STATUS carries the conflict verdict";
    ASSERT_EQ(fixture.session().topics().size(), 2u);
    EXPECT_EQ(fixture.session().topics()[0].status, kTopicStatusOk);
    EXPECT_EQ(fixture.session().topics()[1].status, kTopicStatusNameConflict);
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

TEST(SessionTopics, DuplicateNameAcrossDirectionsIsAConflict)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 2u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 16u, "dup"));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 2u, topic_payload(1u, 17u, "dup", 0x01u));
    bool refused = false;
    for (const auto & unit : fixture.drain_units()) {
    const std::vector<std::uint8_t> decoded = decoded_unit(unit);
    if (decoded.size() >= 5u && decoded[0] == kControl && decoded[2] == 0x05u &&
      decoded[3] == 17u &&
      decoded[4] == kTopicStatusNameConflict)
    {
      refused = true;
    }
    }
    EXPECT_TRUE(refused) << "the second TOPIC_STATUS carries the conflict verdict";
    ASSERT_EQ(fixture.session().topics().size(), 2u);
    EXPECT_EQ(fixture.session().topics()[0].status, kTopicStatusOk);
    EXPECT_EQ(fixture.session().topics()[1].status, kTopicStatusNameConflict);
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

TEST(SessionTopics, AnUnknownTypeKeepsItsVerdictOverARepeatedName)
{
    Fixture fixture([](const TopicInfo & info) {
      return info.topic_id == 17u ? kTopicStatusUnknownType : kTopicStatusOk;
    });
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 2u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 16u, "dup"));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 2u, topic_payload(1u, 17u, "dup", 0x01u));
    bool refused = false;
    for (const auto & unit : fixture.drain_units()) {
    const std::vector<std::uint8_t> decoded = decoded_unit(unit);
    if (decoded.size() >= 5u && decoded[0] == kControl && decoded[2] == 0x05u &&
      decoded[3] == 17u &&
      decoded[4] == kTopicStatusUnknownType)
    {
      refused = true;
    }
    }
    EXPECT_TRUE(refused) << "the unknown type keeps verdict 1 over the repeated name";
    ASSERT_EQ(fixture.session().topics().size(), 2u);
    EXPECT_EQ(fixture.session().topics()[1].status, kTopicStatusUnknownType);
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

TEST(SessionSend, RefusesFramesPastTheMcuCapacity)
{
    ManualClock clock;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};});
    clock.advance(std::chrono::milliseconds(10));
    session.poll();
    std::vector<std::uint8_t> hello{kControl, 0u};
    const std::vector<std::uint8_t> payload = hello_payload(0x1000u, 1u);
    hello.insert(hello.end(), payload.begin(), payload.end());
    transport.script_read(wire_of(with_check(hello)));
    session.poll();
    std::vector<std::uint8_t> topic{kControl, 1u};
    const std::vector<std::uint8_t> announced = topic_payload(0u, 16u, "x");
    topic.insert(topic.end(), announced.begin(), announced.end());
    transport.script_read(wire_of(with_check(topic)));
    session.poll();
    EXPECT_EQ(session.state(), SessionState::kConnected);
    EXPECT_EQ(session.mcu_max_frame(), 512u);
    EXPECT_EQ(session.mcu_rx_capacity(), 515u);
    transport.discard_writes();
    transport.limit_write(0u);
    std::size_t accepted = 0;
    bool refused = false;
    for (int i = 0; i < 200; ++i) {
    if (session.send_user(16u, {0x01, 0x02, 0x03, 0x04})) {
      ++accepted;
    } else {
      refused = true;
      break;
    }
    }
    EXPECT_GT(accepted, 0u) << "the frame in flight always goes out";
    EXPECT_TRUE(refused) << "a wedged link stops the pacer instead of growing the heap";
    EXPECT_LT(accepted, 200u);
    transport.limit_write(1000000u);
    session.poll();
    EXPECT_TRUE(session.send_user(16u, {0x01})) << "the link takes frames again once it drains";
}

// A wedged link must not grow the host heap through the control path: while
// writes block, each control kind keeps one pending frame ([SES-33]).
TEST(SessionControl, CoalescesHelloReqWhenWritesBlock)
{
    Fixture fixture;
    fixture.block_writes();
    fixture.advance_to(10000);
    ASSERT_EQ(fixture.session().state(), SessionState::kHandshaking);
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u) << "sixteen HELLO_REQs collapse into one";
    EXPECT_EQ(decoded_unit(units[0])[2], 0x01u);
}

TEST(SessionControl, CoalescesHeartbeatsWhenWritesBlock)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 16u, "x"));
    fixture.drain_units();
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.block_writes();
    // The MCU heartbeats below only keep the link alive; the host answers
    // with ten of its own, one every 500 ms, all wedged.
    const std::vector<std::uint8_t> counters(8u, 0u);
    for (std::uint32_t at = 1000; at <= 5000; at += 1000) {
    fixture.advance_to(at);
    fixture.deliver_frame(kHeartbeat, 0u, counters);
    }
    fixture.advance_to(5110);
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u) << "ten heartbeats collapse into one";
    EXPECT_EQ(decoded_unit(units[0])[0], kHeartbeat);
    EXPECT_EQ(decoded_unit(units[0])[1], 0u) << "the replacement keeps the slot seq for [FRM-5]";
}

TEST(SessionControl, CoalescesTopicStatusPerTopicWhenWritesBlock)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.block_writes();
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 2u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 16u, "a"));
    // The walk answers every index, even one reusing a topic id: the second
    // verdict for the id replaces the first instead of queueing behind it.
    fixture.deliver_frame(kControl, 2u, topic_payload(1u, 16u, "b"));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.allow_writes();
    fixture.session().poll();
    std::size_t ready = 0;
    std::size_t status16 = 0;
    for (const auto & unit : fixture.drain_units()) {
    const std::vector<std::uint8_t> decoded = decoded_unit(unit);
    if (decoded[0] == kControl && decoded[2] == 0x06u) {
      ++ready;
    }
    if (decoded[0] == kControl && decoded[2] == 0x05u && decoded[3] == 16u) {
      ++status16;
    }
    }
    EXPECT_EQ(status16, 1u) << "one TOPIC_STATUS per topic id";
    EXPECT_EQ(ready, 1u) << "READY is one pending frame, not one per verdict";
}

TEST(SessionControl, CoalescesTopicReqRetriesWhenWritesBlock)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.block_writes();
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    // The first TOPIC_REQ plus three retries, all wedged, all for index 0.
    fixture.advance_to(710);
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 16u, "x"));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.allow_writes();
    fixture.session().poll();
    std::size_t requests = 0;
    std::size_t frames = 0;
    for (const auto & unit : fixture.drain_units()) {
    ++frames;
    const std::vector<std::uint8_t> decoded = decoded_unit(unit);
    if (decoded[0] == kControl && decoded[2] == 0x03u) {
      ++requests;
    }
    }
    EXPECT_EQ(requests, 1u) << "the retries replace one TOPIC_REQ slot";
    EXPECT_EQ(frames, 3u) << "the TOPIC_REQ, the TOPIC_STATUS and READY";
}

TEST(SessionControl, DropsPendingControlOnRehandshake)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 16u, "x"));
    fixture.drain_units();
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.block_writes();
    fixture.advance_to(1010);
    // A reset while two heartbeats are wedged: the walk that follows must not
    // carry the old session's heartbeat with it.
    fixture.deliver_frame(kControl, 2u, hello_payload(0x2000u, 1u));
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u) << "the TOPIC_REQ alone, no stale heartbeat";
    const std::vector<std::uint8_t> decoded = decoded_unit(units[0]);
    EXPECT_EQ(decoded[0], kControl);
    EXPECT_EQ(decoded[2], 0x03u);
}

// The ticket's acceptance: 300 s of wedged session time with heartbeats and
// re-handshakes stays within a constant bound instead of growing steadily.
TEST(SessionControl, BoundsPendingControlOverALongBlockedStretch)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.block_writes();
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 2u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 16u, "a"));
    fixture.deliver_frame(kControl, 2u, topic_payload(1u, 17u, "b"));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    const std::vector<std::uint8_t> counters(8u, 0u);
    std::uint8_t seq = 0;
    for (std::uint32_t at = 1010; at <= 311010; at += 1000) {
    fixture.advance_to(at);
    fixture.deliver_frame(kHeartbeat, seq++, counters);
    if ((at - 1010u) % 30000u == 0u) {
      fixture.deliver_frame(kControl, seq++, hello_payload(0x2000u, 2u));
      fixture.deliver_frame(kControl, seq++, topic_payload(0u, 16u, "a"));
      fixture.deliver_frame(kControl, seq++, topic_payload(1u, 17u, "b"));
    }
    }
    EXPECT_LE(fixture.session().pending_bytes(), 128u);
    fixture.allow_writes();
    fixture.session().poll();
    std::size_t bytes = 0;
    for (const auto & unit : fixture.drain_units()) {
    bytes += unit.size();
    }
    // One slot per singleton plus one TOPIC_STATUS per topic id is below 64
    // bytes for two topics; 128 leaves margin without hiding growth.
    EXPECT_LE(bytes, 128u) << "pending control stays bounded on a wedged link";
}

TEST(SessionControl, CountsReplacedControlFrames)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 16u, "x"));
    fixture.drain_units();
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_EQ(fixture.session().control_coalesced(), 0u);
    fixture.block_writes();
    const std::vector<std::uint8_t> counters(8u, 0u);
    for (std::uint32_t at = 1000; at <= 5000; at += 1000) {
    fixture.advance_to(at);
    fixture.deliver_frame(kHeartbeat, 0u, counters);
    }
    fixture.advance_to(5110);
    EXPECT_EQ(fixture.session().control_coalesced(),
          9u) << "nine of ten heartbeats replaced one slot";
    EXPECT_LE(fixture.session().pending_bytes(), 16u) << "one heartbeat frame stays queued";
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(decoded_unit(units[0])[1], 0u) << "the replacement keeps the slot seq for [FRM-5]";
}

// covers: SES-22, SES-23, SES-25
std::vector<std::uint8_t> pong_payload(std::uint8_t id, std::uint32_t tick)
{
  std::vector<std::uint8_t> payload{0x02u, id};
  for (int shift = 0; shift < 32; shift += 8) {
    payload.push_back(static_cast<std::uint8_t>((tick >> shift) & 0xFFu));
  }
  return payload;
}

std::uint32_t le32(const std::vector<std::uint8_t> & bytes, std::size_t i)
{
  return static_cast<std::uint32_t>(bytes[i]) | (static_cast<std::uint32_t>(bytes[i + 1u]) << 8u) |
         (static_cast<std::uint32_t>(bytes[i + 2u]) << 16u) |
         (static_cast<std::uint32_t>(bytes[i + 3u]) << 24u);
}

void connect_empty(Fixture & fixture)
{
    // The time bit ([CTL-2]) tells the host the MCU answers PINGs.
  fixture.deliver_frame(kControl, 0u, hello_payload(0u, 0u, 4u, 0x01u));
  ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
}

// The PONGs answering one burst: 2 ms after each PING, the 1 MHz tick
// stamped at the arrival, so the round trip is symmetric.
void answer_pings(Fixture & fixture, std::uint32_t ready_at)
{
  for (std::uint8_t k = 0; k < 8; ++k) {
    const std::uint32_t at = ready_at + 50u * k + 2u;
    fixture.advance_to(at);
    fixture.deliver_frame(0x01u, k, pong_payload(k, (ready_at + 50u * k + 1u) * 1000u));
  }
}

// Topic, seq and payload of the time-channel units since the last drain,
// without their check; other topics are skipped.
struct TimeFrame
{
  std::uint8_t seq = 0;
  std::vector<std::uint8_t> payload;
};

std::vector<TimeFrame> drain_time(Fixture & fixture, std::uint8_t opcode = 0xFFu)
{
  std::vector<TimeFrame> out;
  for (auto & unit : fixture.drain_units()) {
    const std::vector<std::uint8_t> decoded = decoded_unit(unit);
    if (decoded.size() >= 4u && decoded[0] == 0x01u && (opcode == 0xFFu || decoded[2] == opcode)) {
      TimeFrame frame;
      frame.seq = decoded[1];
      frame.payload.assign(decoded.begin() + 2, decoded.end() - 2);
      out.push_back(frame);
    }
  }
  return out;
}

TEST(SessionTimeSync, SendsEightPings50msApartAfterReady)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    // READY and the first PING leave in the same poll.
    const auto first = fixture.drain_units();
    ASSERT_EQ(first.size(), 2u);
    EXPECT_EQ(hex_of(first[1]), hex_of(time_wire(0u, {0x01u, 0u})));
    for (std::uint8_t k = 1; k < 8; ++k) {
    fixture.advance_to(100u + 50u * k);
    const auto units = drain_time(fixture);
    ASSERT_EQ(units.size(), 1u) << "PING " << static_cast<int>(k);
    EXPECT_EQ(units[0].seq, k);
    EXPECT_EQ(units[0].payload, std::vector<std::uint8_t>({0x01u, k}));
    }
    fixture.advance_to(500);
    EXPECT_TRUE(drain_time(fixture).empty()) << "the burst is eight PINGs";
}

TEST(SessionTimeSync, PacesSteadyPingsThenTheConfiguredPeriod)
{
    SessionConfig config;
    config.time_sync_period_ms = 2000;
    // The cadence is what this pins down; a generous timeout keeps the
    // session connected while no MCU frame arrives.
    config.link_timeout_ms = 3600000;
    Fixture fixture([](const TopicInfo &) {return std::uint8_t{0};}, config);
    fixture.advance_to(100);
    connect_empty(fixture);
    fixture.drain_units();
    fixture.advance_to(450);
    EXPECT_EQ(drain_time(fixture).size(), 7u) << "the rest of the burst";
    const std::uint32_t steady[] = {1450u, 2450u, 3450u, 4450u, 5450u, 6450u, 7450u, 8450u, 9450u};
    for (std::uint32_t at : steady) {
    fixture.advance_to(at);
    const auto units = drain_time(fixture);
    ASSERT_EQ(units.size(), 1u) << "steady PING at " << at;
    EXPECT_EQ(units[0].payload[0], 0x01u);
    }
    fixture.advance_to(10449);
    EXPECT_TRUE(drain_time(fixture).empty());
    fixture.advance_to(10450);
    EXPECT_EQ(drain_time(fixture).size(), 1u) << "the last 1000 ms step";
    fixture.advance_to(12449);
    EXPECT_TRUE(drain_time(fixture).empty());
    fixture.advance_to(12450);
    EXPECT_EQ(drain_time(fixture).size(), 1u) << "period_ms once connected for 10 s";
}

TEST(SessionTimeSync, CorrectsAfterTheBurstFromPongs)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    fixture.drain_units();
    answer_pings(fixture, 100);
    const auto corrections = drain_time(fixture, 0x03u);
    ASSERT_EQ(corrections.size(), 1u) << "one CORRECTION once the burst is done";
    const TimeFrame & correction = corrections[0];
    EXPECT_EQ(correction.seq, 8u);
    ASSERT_EQ(correction.payload.size(), 15u);
    EXPECT_EQ(correction.payload[0], 0x03u);
    EXPECT_EQ(le32(correction.payload, 1u), 451000u) << "tick0 is the latest PONG tick";
    EXPECT_EQ(le32(correction.payload, 5u), 0u) << "sec";
    EXPECT_EQ(le32(correction.payload, 9u), 451000000u) << "nanosec: the arrival midpoint";
    const std::int16_t drift = static_cast<std::int16_t>(
    correction.payload[13u] | (static_cast<std::uint16_t>(correction.payload[14u]) << 8u));
    EXPECT_EQ(drift, 0);
    EXPECT_TRUE(fixture.session().time_available());
    EXPECT_EQ(fixture.session().time_offset_ns(), 0);
    EXPECT_EQ(fixture.session().time_drift_ppm(), 0);
    EXPECT_EQ(fixture.session().time_round_trip_ns(), 2000000);
}

TEST(SessionTimeSync, IgnoresStrayAndHandshakePongs)
{
    Fixture fixture;
    fixture.advance_to(100);
    // A PONG before READY answers nothing the host sent.
    fixture.deliver_frame(0x01u, 0u, pong_payload(0u, 100000u));
    connect_empty(fixture);
    fixture.drain_units();
    // An id the host never sent carries no sample.
    fixture.deliver_frame(0x01u, 0u, pong_payload(9u, 200000u));
    fixture.advance_to(450);
    EXPECT_TRUE(drain_time(fixture, 0x03u).empty()) << "no CORRECTION without a matched PONG";
    // An unknown time opcode is ignored like the other system channels.
    fixture.deliver_frame(0x01u, 1u, std::vector<std::uint8_t>{0x07u});
    fixture.advance_to(1450);
    EXPECT_EQ(drain_time(fixture, 0x01u).size(), 1u) << "steady PINGs continue";
    EXPECT_FALSE(fixture.session().time_available());
}

TEST(SessionTimeSync, ResendsTheCorrectionOnAMoveAndEvery5s)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    fixture.drain_units();
    answer_pings(fixture, 100);
    ASSERT_EQ(drain_time(fixture, 0x03u).size(), 1u);
    // The MCU clock jumps 300 us: the next sample misses the prediction from
    // the first correction by the jump, past the 100 us bar.
    fixture.advance_to(1452);
    fixture.deliver_frame(0x01u, 0u, pong_payload(8u, 1451300u));
    // Sixteen jumped samples settle the window: the slope returns to zero
    // and the last correction anchors the latest arrival midpoint exactly.
    for (std::uint8_t k = 1; k < 16; ++k) {
    const std::uint32_t at = 1450u + 1000u * k + 2u;
    fixture.advance_to(at);
    fixture.deliver_frame(0x01u, k, pong_payload(8u + k, (at - 1u) * 1000u + 300u));
    }
    auto moved = drain_time(fixture, 0x03u);
    EXPECT_GE(moved.size(), 1u) << "the jump corrects again";
    ASSERT_FALSE(moved.empty());
    EXPECT_EQ(le32(moved.front().payload, 1u), 1451300u) << "the first jump corrects at once";
    EXPECT_EQ(le32(moved.front().payload, 5u), 1u);
    // The 5 s floor: with no new PONGs the estimator stands still, and MCU
    // heartbeats keep the link from timing out, so the next correction
    // repeats the last one within 5 s of it.
    TimeFrame floor_correction;
    bool resent = false;
    std::uint8_t hb_seq = 0;
    const std::vector<std::uint8_t> counters(8u, 0u);
    for (std::uint32_t t = 16453; t <= 26453 && !resent; ++t) {
    fixture.advance_to(t);
    if (t % 1000u == 0u) {
      fixture.deliver_frame(kHeartbeat, hb_seq++, counters);
    }
    auto more = drain_time(fixture, 0x03u);
    if (!more.empty()) {
      floor_correction = more.back();
      resent = true;
    }
    }
    EXPECT_TRUE(resent) << "a CORRECTION at least every 5 s";
    EXPECT_EQ(floor_correction.payload, moved.back().payload);
}

TEST(SessionHandshake, IgnoresATopicOutsideTheWalk)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    fixture.drain_units();
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_TRUE(fixture.drain_units().empty()) << "a stray TOPIC answers nothing";
}

TEST(SessionLink, IgnoresAnEmptyControlFrame)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    fixture.deliver_frame(kControl, 0u, std::vector<std::uint8_t>{});
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

TEST(SessionLink, IgnoresAnUnknownControlOpcode)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    fixture.deliver_frame(kControl, 0u, std::vector<std::uint8_t>{0x7Fu});
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

TEST(SessionLink, IgnoresAShortTimeFrame)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    fixture.deliver_frame(0x01u, 0u, std::vector<std::uint8_t>{0x00u});
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

TEST(SessionLink, IgnoresASystemChannelFrame)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    const std::uint32_t before = fixture.session().resets();
    fixture.deliver_frame(0x05u, 0u, std::vector<std::uint8_t>{0x01u});
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_EQ(fixture.session().resets(), before) << "later system traffic is not a reset";
}

TEST(SessionConnected, QueuesHostFramesForTheMcu)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    fixture.drain_units();
    EXPECT_TRUE(fixture.session().send_user(0x10u, {0x01u, 0x02u}));
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(hex_of(units[0]), hex_of(wire_of(with_check({0x10u, 0u, 0x01u, 0x02u}))))
        << "the user topic rides its own frame";
}

TEST(SessionTimeSync, SendsNothingWhenDisabled)
{
    SessionConfig config;
    config.time_sync_enabled = false;
    Fixture fixture([](const TopicInfo &) {return std::uint8_t{0};}, config);
    fixture.advance_to(100);
    connect_empty(fixture);
    // A PONG answers nothing while disabled.
    fixture.deliver_frame(0x01u, 0u, pong_payload(0u, 100000u));
    fixture.advance_to(2100);
    for (auto & unit : fixture.drain_units()) {
    EXPECT_NE(decoded_unit(unit)[0], 0x01u) << "no PING and no CORRECTION";
    }
    EXPECT_FALSE(fixture.session().time_available());
}

TEST(SessionTimeSync, SendsNothingWhenTheMcuLacksTheTimeBit)
{
    Fixture fixture;
    fixture.advance_to(100);
    // The HELLO leaves the time bit ([CTL-2]) clear: the firmware answers
    // no PING, so the host sends none.
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 0u));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.drain_units();
    fixture.advance_to(2100);
    for (auto & unit : fixture.drain_units()) {
    EXPECT_NE(decoded_unit(unit)[0], 0x01u) << "no PING without the time bit";
    }
    EXPECT_FALSE(fixture.session().time_available());
}

TEST(SessionTimeSync, RefusesAPeriodOutside1To5000ms)
{
    ManualClock clock;
    ManualClock ros;
    FakeTransport transport;
    const auto open = [&transport]() {return std::unique_ptr<Transport>(new Link(transport));};
    const auto resolve = [](const TopicInfo &) {return std::uint8_t{0};};
    for (int bad : {0, -1, 6000}) {
    SessionConfig config;
    config.time_sync_period_ms = bad;
    EXPECT_THROW(Session(config, clock, ros, open, resolve),
            std::invalid_argument) << "period " << bad;
    }
    for (int good : {1, 5000}) {
    SessionConfig config;
    config.time_sync_period_ms = good;
    EXPECT_NO_THROW(Session(config, clock, ros, open, resolve)) << "period " << good;
    }
}

TEST(SessionTimeSync, RestartsBurstAndEstimateOnReset)
{
    Fixture fixture;
    fixture.advance_to(100);
    connect_empty(fixture);
    fixture.drain_units();
    answer_pings(fixture, 100);
    EXPECT_EQ(drain_time(fixture, 0x03u).size(), 1u);
    EXPECT_TRUE(fixture.session().time_available());
    fixture.deliver_frame(kControl, 0u, hello_payload(1u, 0u, 4u, 0x01u));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_FALSE(fixture.session().time_available()) << "the re-handshake clears the estimate";
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 2u) << "READY, then the restarted burst";
    EXPECT_EQ(hex_of(units[1]), hex_of(time_wire(0u, {0x01u, 0u})));
}

// One log payload ([LOG-1]): the level, the text as a string, the value kind,
// then the value bytes. Short texts fit one varint byte.
std::vector<std::uint8_t> log_payload(
  std::uint8_t level, const std::string & text, std::uint8_t kind = 0u,
  const std::vector<std::uint8_t> & value = {})
{
  std::vector<std::uint8_t> payload{level, static_cast<std::uint8_t>(text.size())};
  payload.insert(payload.end(), text.begin(), text.end());
  payload.push_back(kind);
  payload.insert(payload.end(), value.begin(), value.end());
  return payload;
}

// A session whose MCU log lines are captured with their severity.
class LogFixture
{
public:
  LogFixture()
  {
    SessionEvents events;
    events.mcu_log = [this](SessionLog level, const std::string & text) {
        lines_.emplace_back(level, text);
      };
    session_ = std::make_unique<Session>(
            SessionConfig{}, clock_, clock_,
      [this]() {return std::unique_ptr<Transport>(new Link(transport_));},
      [](const TopicInfo &) {return std::uint8_t{0};}, events);
  }

  void deliver_log(const std::vector<std::uint8_t> & payload)
  {
    std::vector<std::uint8_t> frame{kLogTopic, 0u};
    frame.insert(frame.end(), payload.begin(), payload.end());
    transport_.script_read(wire_of(with_check(frame)));
    session_->poll();
  }

  const std::vector<std::pair<SessionLog, std::string>> & lines() const
  {
    return lines_;
  }

private:
  static constexpr std::uint8_t kLogTopic = 2u;
  ManualClock clock_;
  FakeTransport transport_;
  std::unique_ptr<Session> session_;
  std::vector<std::pair<SessionLog, std::string>> lines_;
};

TEST(SessionLog, ForwardsEachLevelWithItsSeverity)
{
    struct Case
  {
    std::uint8_t wire;
    SessionLog severity;
    };
    const Case cases[] = {{10u, SessionLog::kDebug},
    {20u, SessionLog::kInfo},
    {30u, SessionLog::kWarning},
    {40u, SessionLog::kError},
    {50u, SessionLog::kFatal}};
    for (const Case & line : cases) {
    LogFixture fixture;
    fixture.deliver_log(log_payload(line.wire, "line"));
    ASSERT_EQ(fixture.lines().size(), 1u) << "level " << static_cast<int>(line.wire);
    EXPECT_EQ(fixture.lines()[0].first, line.severity) << "level " << static_cast<int>(line.wire);
    EXPECT_EQ(fixture.lines()[0].second, "line") << "level " << static_cast<int>(line.wire);
    }
}

TEST(SessionLog, AppendsEachValueKindAfterTheText)
{
    struct Case
  {
    std::uint8_t kind;
    std::vector<std::uint8_t> bytes;
    std::string rendered;
    };
    const Case cases[] = {{1u, {0x34u, 0x12u}, "v=4660"},
    {2u, {0xFEu, 0xFFu}, "v=-2"},
    {3u, {0x78u, 0x56u, 0x34u, 0x12u}, "v=305419896"},
    {4u, {0xFFu, 0xFFu, 0xFFu, 0xFFu}, "v=-1"},
    {5u, {0x00u, 0x00u, 0xC0u, 0x3Fu}, "v=1.5"}};
    for (const Case & line : cases) {
    LogFixture fixture;
    fixture.deliver_log(log_payload(20u, "v=", line.kind, line.bytes));
    ASSERT_EQ(fixture.lines().size(), 1u) << "kind " << static_cast<int>(line.kind);
    EXPECT_EQ(fixture.lines()[0].first,
            SessionLog::kInfo) << "kind " << static_cast<int>(line.kind);
    EXPECT_EQ(fixture.lines()[0].second, line.rendered) << "kind " << static_cast<int>(line.kind);
    }
}

TEST(SessionLog, RefusesALogFrameWithAReservedValueKind)
{
    LogFixture fixture;
    fixture.deliver_log(log_payload(20u, "line", 6u, {0x00u}));
    EXPECT_TRUE(fixture.lines().empty()) << "a reserved kind forwards nothing";
}

TEST(SessionLog, DropsALogFrameWithAnUnknownLevel)
{
    LogFixture fixture;
    fixture.deliver_log(log_payload(25u, "line"));
    EXPECT_TRUE(fixture.lines().empty()) << "an unknown level forwards nothing";
}

TEST(SessionLog, DropsATruncatedLogFrame)
{
    struct Case
  {
    std::vector<std::uint8_t> payload;
    };
    const Case cases[] = {{{}},
    {{20u}},
    {{20u, 2u, 'v', '='}},
    {log_payload(20u, "v=", 1u, {0x34u})},
    {log_payload(20u, "v=", 2u, {0x34u})},
    {log_payload(20u, "v=", 3u, {0x34u, 0x12u, 0x00u})},
    {log_payload(20u, "v=", 4u, {0x34u, 0x12u, 0x00u})},
    {log_payload(20u, "v=", 5u, {0x00u, 0x00u, 0xC0u})}};
    for (const Case & line : cases) {
    LogFixture fixture;
    fixture.deliver_log(line.payload);
    EXPECT_TRUE(fixture.lines().empty()) << "a short payload forwards nothing";
    }
}

TEST(SessionLog, DropsALogFrameWithTrailingBytes)
{
    struct Case
  {
    std::uint8_t kind;
    std::vector<std::uint8_t> bytes;
    };
    const Case cases[] = {{0u, {}},
    {1u, {0x34u, 0x12u}},
    {2u, {0xFEu, 0xFFu}},
    {3u, {0x78u, 0x56u, 0x34u, 0x12u}},
    {4u, {0xFFu, 0xFFu, 0xFFu, 0xFFu}},
    {5u, {0x00u, 0x00u, 0xC0u, 0x3Fu}}};
    for (const Case & line : cases) {
    std::vector<std::uint8_t> bytes = line.bytes;
    bytes.push_back(0x00u);
    LogFixture fixture;
    fixture.deliver_log(log_payload(20u, "v=", line.kind, bytes));
    EXPECT_TRUE(fixture.lines().empty()) << "kind " << static_cast<int>(line.kind);
    }
}

TEST(SessionLog, IgnoresLogLinesWithoutAMcuSink)
{
    std::vector<std::string> logged;
    SessionEvents events;
    events.log = [&logged](SessionLog, const std::string & text) {logged.push_back(text);};
    ManualClock clock;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};}, events);
    std::vector<std::uint8_t> frame{2u, 0u};
    const std::vector<std::uint8_t> payload = log_payload(20u, "line");
    frame.insert(frame.end(), payload.begin(), payload.end());
    transport.script_read(wire_of(with_check(frame)));
    session.poll();
    EXPECT_TRUE(logged.empty()) << "a log line never reaches the bridge sink";
}

TEST(SessionLog, DropsALogTextWithAnEmbeddedNul)
{
    LogFixture fixture;
    fixture.deliver_log(log_payload(20u, std::string("a\0b", 3u)));
    EXPECT_TRUE(fixture.lines().empty()) << "a NUL byte refuses the line";
}

// An MCU error report ([ERR-1]): the code, the topic, the count and the
// field, decoded in any state like a log line.
// covers: ERR-1, ERR-2, ERR-6
class ErrorFixture
{
public:
  ErrorFixture()
  {
    SessionEvents events;
    events.mcu_log = [this](SessionLog level, const std::string & text) {
        lines_.emplace_back(level, text);
      };
    session_ = std::make_unique<Session>(
            SessionConfig{}, clock_, clock_,
      [this]() {return std::unique_ptr<Transport>(new Link(transport_));},
      [](const TopicInfo &) {return std::uint8_t{0};}, events);
  }

  void deliver_report(const std::vector<std::uint8_t> & payload)
  {
    std::vector<std::uint8_t> frame{kErrorTopic, 0u};
    frame.insert(frame.end(), payload.begin(), payload.end());
    transport_.script_read(wire_of(with_check(frame)));
    session_->poll();
  }

  const std::vector<std::pair<SessionLog, std::string>> & lines() const
  {
    return lines_;
  }

  std::array<std::uint64_t, 6> error_sums() const
  {
    return session_->error_sums();
  }

  std::array<std::uint64_t, 6> error_counts(std::uint8_t topic_id) const
  {
    return session_->error_counts(topic_id);
  }

private:
  static constexpr std::uint8_t kErrorTopic = 5u;
  ManualClock clock_;
  FakeTransport transport_;
  std::unique_ptr<Session> session_;
  std::vector<std::pair<SessionLog, std::string>> lines_;
};

TEST(SessionError, ForwardsACodeWithItsTopicAndCount)
{
    ErrorFixture fixture;
    fixture.deliver_report({6u, 16u, 42u, 0u, 0xFFu});
    ASSERT_EQ(fixture.lines().size(), 1u) << "one report forwards one line";
    EXPECT_EQ(fixture.lines()[0].first, SessionLog::kWarning);
    const std::string & line = fixture.lines()[0].second;
    EXPECT_NE(line.find("code 6"), std::string::npos) << line;
    EXPECT_NE(line.find("16"), std::string::npos) << line;
    EXPECT_NE(line.find("42"), std::string::npos) << line;
}

TEST(SessionError, DropsAShortReport)
{
    ErrorFixture fixture;
    fixture.deliver_report({6u, 16u, 42u, 0u});
    EXPECT_TRUE(fixture.lines().empty()) << "a short payload forwards nothing";
}

// An MCU error report count ([ERR-6]) adds to the running sum of its code,
// and to the per-topic count when the report names a topic.
// covers: ERR-6
TEST(SessionError, AccumulatesCodeCountsAcrossReports)
{
    ErrorFixture fixture;
    fixture.deliver_report({6u, 16u, 42u, 0u, 0xFFu});
    fixture.deliver_report({6u, 0xFFu, 3u, 0u, 0xFFu});
    fixture.deliver_report({1u, 16u, 5u, 0u, 0x00u});
    const std::array<std::uint64_t, 6> sums = fixture.error_sums();
    EXPECT_EQ(sums[5], 45u) << "both code 6 deltas add up whatever topic they name";
    EXPECT_EQ(sums[0], 5u);
    EXPECT_EQ(sums[1], 0u);
    EXPECT_EQ(sums[2], 0u);
    EXPECT_EQ(sums[3], 0u);
    EXPECT_EQ(sums[4], 0u);
    EXPECT_EQ(fixture.error_counts(16u)[5], 42u) << "only the topic-naming report counts per topic";
    EXPECT_EQ(fixture.error_counts(16u)[0], 5u);
    EXPECT_EQ(fixture.error_counts(17u)[5], 0u) << "an unnamed topic reads zero";
}

// A report with topic id 0xFF names no topic, so it feeds the global sum of
// its code only and leaves every per-topic count at zero.
// covers: ERR-1, ERR-6
TEST(SessionError, CountsUntargetedReportsGloballyOnly)
{
    ErrorFixture fixture;
    fixture.deliver_report({3u, 0xFFu, 7u, 0u, 0xFFu});
    EXPECT_EQ(fixture.error_sums()[2], 7u);
    EXPECT_EQ(fixture.error_counts(0xFFu)[2], 0u) << "the 0xFF sentinel is not a topic";
    EXPECT_EQ(fixture.error_counts(16u)[2], 0u);
}

// A count of 0xFFFF followed by 1 adds 65536: the report count wraps, the
// running sum does not.
// covers: ERR-6
TEST(SessionError, AccumulatesAcrossAWrappedCount)
{
    ErrorFixture fixture;
    fixture.deliver_report({4u, 16u, 0xFFu, 0xFFu, 0xFFu});
    fixture.deliver_report({4u, 16u, 1u, 0u, 0xFFu});
    EXPECT_EQ(fixture.error_sums()[3], 65536u);
    EXPECT_EQ(fixture.error_counts(16u)[3], 65536u);
}

TEST(SessionError, DropsAReportWithAnUnknownCode)
{
    for (const std::uint8_t code : {0u, 7u, 0xFFu}) {
    ErrorFixture fixture;
    fixture.deliver_report({code, 16u, 1u, 0u, 0xFFu});
    EXPECT_TRUE(fixture.lines().empty()) << "code " << static_cast<int>(code) <<
      " forwards nothing";
    }
}

// covers: SYS-5
TEST(SessionError, DropsAReportWithTrailingBytes)
{
    ErrorFixture fixture;
    fixture.deliver_report({6u, 16u, 1u, 0u, 0xFFu, 0x00u});
    EXPECT_TRUE(fixture.lines().empty()) << "a long payload forwards nothing";
}

// covers: SYS-5
TEST(SessionError, DropsAnEmptyReport)
{
    ErrorFixture fixture;
    fixture.deliver_report({});
    EXPECT_TRUE(fixture.lines().empty()) << "an empty payload forwards nothing";
}

// covers: SYS-5
TEST(SessionError, DropsAOneByteReport)
{
    ErrorFixture fixture;
    fixture.deliver_report({6u});
    EXPECT_TRUE(fixture.lines().empty()) << "a one-byte payload forwards nothing";
}

// covers: SYS-5
TEST(SessionError, DropsAThreeByteReport)
{
    ErrorFixture fixture;
    fixture.deliver_report({6u, 16u, 42u});
    EXPECT_TRUE(fixture.lines().empty()) << "a three-byte payload forwards nothing";
}

// covers: ERR-1, ERR-2
TEST(SessionError, UsesTheSingularOccurrenceForACountOfOne)
{
    ErrorFixture fixture;
    fixture.deliver_report({2u, 20u, 1u, 0u, 0xFFu});
    ASSERT_EQ(fixture.lines().size(), 1u) << "one report forwards one line";
    EXPECT_NE(fixture.lines()[0].second.find("1 occurrence"),
          std::string::npos) << fixture.lines()[0].second;
    EXPECT_EQ(fixture.lines()[0].second.find("1 occurrences"), std::string::npos)
        << fixture.lines()[0].second;
}

// covers: ERR-5
TEST(SessionError, NamesTheTopicAndFieldPathWhenTheyResolve)
{
    std::vector<std::pair<SessionLog, std::string>> lines;
    SessionEvents events;
    events.mcu_log = [&lines](SessionLog level, const std::string & text) {
      lines.emplace_back(level, text);
    };
    ManualClock clock;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};}, events);
    const auto deliver = [&transport, &session](std::uint8_t topic_id, std::uint8_t seq,
    const std::vector<std::uint8_t> & payload) {
      std::vector<std::uint8_t> decoded{topic_id, seq};
      decoded.insert(decoded.end(), payload.begin(), payload.end());
      transport.script_read(wire_of(with_check(decoded)));
      session.poll();
    };
    deliver(0u, 0u, hello_payload(0x1000u, 1u));
    deliver(0u, 1u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(session.state(), SessionState::kConnected) << "one ok topic connects at once";
    std::vector<std::uint8_t> report{1u, 0x10u, 7u, 0u, 0u};
    deliver(5u, 0u, report);
    ASSERT_EQ(lines.size(), 1u) << "one report forwards one line";
    EXPECT_NE(lines[0].second.find("code 1"), std::string::npos) << lines[0].second;
    EXPECT_NE(lines[0].second.find("battery"), std::string::npos) << lines[0].second;
    EXPECT_NE(lines[0].second.find("data"), std::string::npos) << lines[0].second;
}

// covers: ERR-1, ERR-2
TEST(SessionError, NamesAKnownTopicForAnotherCode)
{
    std::vector<std::pair<SessionLog, std::string>> lines;
    SessionEvents events;
    events.mcu_log = [&lines](SessionLog level, const std::string & text) {
      lines.emplace_back(level, text);
    };
    ManualClock clock;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};}, events);
    const auto deliver = [&transport, &session](std::uint8_t topic_id, std::uint8_t seq,
    const std::vector<std::uint8_t> & payload) {
      std::vector<std::uint8_t> decoded{topic_id, seq};
      decoded.insert(decoded.end(), payload.begin(), payload.end());
      transport.script_read(wire_of(with_check(decoded)));
      session.poll();
    };
    deliver(0u, 0u, hello_payload(0x1000u, 1u));
    deliver(0u, 1u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(session.state(), SessionState::kConnected) << "one ok topic connects at once";
    deliver(5u, 0u, std::vector<std::uint8_t>{2u, 0x10u, 3u, 0u, 0xFFu});
    ASSERT_EQ(lines.size(), 1u) << "one report forwards one line";
    EXPECT_NE(lines[0].second.find("code 2"), std::string::npos) << lines[0].second;
    EXPECT_NE(lines[0].second.find("battery"), std::string::npos) << lines[0].second;
    EXPECT_EQ(lines[0].second.find("at field"), std::string::npos) << lines[0].second;
}

// covers: ERR-1, ERR-2
TEST(SessionError, OmitsTheFieldWhenTheRuleNamesNone)
{
    std::vector<std::pair<SessionLog, std::string>> lines;
    SessionEvents events;
    events.mcu_log = [&lines](SessionLog level, const std::string & text) {
      lines.emplace_back(level, text);
    };
    ManualClock clock;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};}, events);
    const auto deliver = [&transport, &session](std::uint8_t topic_id, std::uint8_t seq,
    const std::vector<std::uint8_t> & payload) {
      std::vector<std::uint8_t> decoded{topic_id, seq};
      decoded.insert(decoded.end(), payload.begin(), payload.end());
      transport.script_read(wire_of(with_check(decoded)));
      session.poll();
    };
    deliver(0u, 0u, hello_payload(0x1000u, 1u));
    deliver(0u, 1u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(session.state(), SessionState::kConnected) << "one ok topic connects at once";
    deliver(5u, 0u, std::vector<std::uint8_t>{1u, 0x10u, 1u, 0u, 0xFFu});
    ASSERT_EQ(lines.size(), 1u) << "one report forwards one line";
    EXPECT_NE(lines[0].second.find("battery"), std::string::npos) << lines[0].second;
    EXPECT_EQ(lines[0].second.find("at field"), std::string::npos) << lines[0].second;
}

// covers: ERR-1, ERR-2
TEST(SessionError, NamesAnUnknownTopicWhenTopicsAreKnown)
{
    std::vector<std::pair<SessionLog, std::string>> lines;
    SessionEvents events;
    events.mcu_log = [&lines](SessionLog level, const std::string & text) {
      lines.emplace_back(level, text);
    };
    ManualClock clock;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};}, events);
    const auto deliver = [&transport, &session](std::uint8_t topic_id, std::uint8_t seq,
    const std::vector<std::uint8_t> & payload) {
      std::vector<std::uint8_t> decoded{topic_id, seq};
      decoded.insert(decoded.end(), payload.begin(), payload.end());
      transport.script_read(wire_of(with_check(decoded)));
      session.poll();
    };
    deliver(0u, 0u, hello_payload(0x1000u, 1u));
    deliver(0u, 1u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(session.state(), SessionState::kConnected) << "one ok topic connects at once";
    deliver(5u, 0u, std::vector<std::uint8_t>{2u, 20u, 1u, 0u, 0xFFu});
    ASSERT_EQ(lines.size(), 1u) << "one report forwards one line";
    EXPECT_NE(lines[0].second.find("20"), std::string::npos) << lines[0].second;
    EXPECT_EQ(lines[0].second.find("at field"), std::string::npos) << lines[0].second;
}

// covers: ERR-1, ERR-5
TEST(SessionError, FallsBackToTheFieldIndexWhenThePathMisses)
{
    std::vector<std::pair<SessionLog, std::string>> lines;
    SessionEvents events;
    events.mcu_log = [&lines](SessionLog level, const std::string & text) {
      lines.emplace_back(level, text);
    };
    ManualClock clock;
    FakeTransport transport;
    Session session(
    SessionConfig{}, clock, clock,
    [&transport]() {return std::unique_ptr<Transport>(new Link(transport));},
    [](const TopicInfo &) {return std::uint8_t{0};}, events);
    const auto deliver = [&transport, &session](std::uint8_t topic_id, std::uint8_t seq,
    const std::vector<std::uint8_t> & payload) {
      std::vector<std::uint8_t> decoded{topic_id, seq};
      decoded.insert(decoded.end(), payload.begin(), payload.end());
      transport.script_read(wire_of(with_check(decoded)));
      session.poll();
    };
    deliver(0u, 0u, hello_payload(0x1000u, 1u));
    deliver(0u, 1u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(session.state(), SessionState::kConnected) << "one ok topic connects at once";
    deliver(5u, 0u, std::vector<std::uint8_t>{1u, 0x10u, 1u, 0u, 5u});
    ASSERT_EQ(lines.size(), 1u) << "one report forwards one line";
    EXPECT_NE(lines[0].second.find("at field '#5'"), std::string::npos) << lines[0].second;
}

TEST(SessionHandshake, RefusesATruncatedHelloAtEveryField)
{
    Fixture fixture;
    fixture.advance_to(10);
    const std::vector<std::uint8_t> full = hello_payload(0x1000u, 1u);
    for (std::size_t len = 1u; len < full.size(); ++len) {
    fixture.deliver_frame(kControl, 0u,
            std::vector<std::uint8_t>(full.begin(), full.begin() + len));
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait)
            << "a hello cut at " << len << " refuses";
    EXPECT_FALSE(fixture.session().seen_hello()) << "a hello cut at " << len << " records nothing";
    }
}

TEST(SessionHandshake, AnswersAnExtraTopicAfterTheHandshake)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u));
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.deliver_frame(kControl, 0u, topic_payload(1u, 0x11u, "extra"));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected)
        << "a TOPIC outside the walk never reopens it";
}

TEST(SessionHandshake, RefusesATruncatedTopic)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u));
    fixture.deliver_frame(kControl, 0u, std::vector<std::uint8_t>{0x04u});
    fixture.deliver_frame(kControl, 0u, std::vector<std::uint8_t>{0x04u, 0x00u});
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking)
        << "a TOPIC cut inside its index never answers the walk";
}

TEST(SessionHandshake, AsksForAHelloAfterBootWait)
{
    Fixture fixture;
    fixture.advance_to(2600);
    const auto units = fixture.drain_units();
    ASSERT_FALSE(units.empty()) << "the boot wait ends in a HELLO_REQ";
    EXPECT_EQ(units[0][2], 0x01u) << "the first control frame asks for a HELLO";
}

TEST(SessionTime, IgnoresTimeFramesBeforeTheHandshake)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver(with_check({0x01u, 0u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}));
    EXPECT_EQ(fixture.session().state(),
          SessionState::kBootWait) << "time frames wait for the session";
}

TEST(SessionTime, IgnoresNonPongTimeFrames)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u, 4u, 0x01u));
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.deliver(with_check({0x01u, 0u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u}));
    fixture.deliver(with_check({0x01u, 1u, 0x07u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected)
        << "a short frame and a foreign id never touch the estimator";
}

TEST(SessionTime, SendsAPingOnceTimeSyncIsUp)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u, 4u, 0x01u));
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    bool pinged = false;
    for (const auto & unit : fixture.drain_units()) {
    const std::vector<std::uint8_t> decoded = decoded_of(unit);
    pinged = pinged || (decoded.size() == 6u && decoded[0] == 0x01u && decoded[2] == 0x01u);
    }
    EXPECT_TRUE(pinged) << "the first connected poll sends a PING";
}

TEST(SessionTime, PublishesACorrectionAfterEightPongs)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u, 4u, 0x01u));
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_FALSE(fixture.session().time_available()) << "no estimate before the first PONG";
    const auto ping_id_of = [](const std::vector<std::uint8_t> & decoded) -> int {
      if (decoded.size() == 6u && decoded[0] == 0x01u && decoded[2] == 0x01u) {
        return decoded[3];
      }
      return -1;
    };
    std::uint32_t tick = 0;
    int answers = 0;
    for (int step = 0; step < 200 && answers < 8; ++step) {
    fixture.advance_to(fixture.now_ms() + 10u);
    for (const auto & unit : fixture.drain_units()) {
      const int id = ping_id_of(decoded_of(unit));
      if (id < 0) {
        continue;
      }
      tick += 50000u;
      std::vector<std::uint8_t> pong = with_check({0x01u, 0u, 0x02u, static_cast<std::uint8_t>(id),
            static_cast<std::uint8_t>(tick & 0xFFu),
            static_cast<std::uint8_t>((tick >> 8u) & 0xFFu),
            static_cast<std::uint8_t>((tick >> 16u) & 0xFFu),
            static_cast<std::uint8_t>((tick >> 24u) & 0xFFu)});
      fixture.deliver(pong);
      ++answers;
    }
    }
    ASSERT_EQ(answers, 8) << "every burst PING gets its PONG";
    EXPECT_TRUE(fixture.session().time_available()) << "eight consistent PONGs anchor a correction";
    bool corrected = false;
    for (const auto & unit : fixture.drain_units()) {
    const std::vector<std::uint8_t> decoded = decoded_of(unit);
    corrected = corrected || (decoded.size() > 2u && decoded[0] == 0x01u && decoded[2] == 0x03u);
    }
    EXPECT_TRUE(corrected) << "the estimate goes out as a CORRECTION";
}

TEST(SessionFrame, IgnoresARuntFrame)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver({0x10u, 0x00u, 0xAAu});
    EXPECT_EQ(fixture.session().state(),
          SessionState::kBootWait) << "a frame under four bytes never parses";
}

TEST(SessionConnected, DropsAUserFrameWithNoRoute)
{
    Fixture fixture([](const TopicInfo &) {
      return std::uint8_t{0};
    }, SessionConfig(), SessionEvents{});
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0u, 1u));
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.deliver_frame(0x30u, 0u, std::vector<std::uint8_t>{0x01u});
    fixture.deliver_frame(0x10u, 1u, std::vector<std::uint8_t>{0x02u});
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected)
        << "an unrouted topic and a missing handler both drop the frame";
}

TEST(SessionLink, TreatsAThrowingOpenAsAClosedPort)
{
    ManualClock clock;
    Session session(
    SessionConfig{}, clock, clock,
    []() -> std::unique_ptr<Transport> {throw std::runtime_error("no port");},
    [](const TopicInfo &) {return std::uint8_t{0};}, SessionEvents{});
    session.poll();
    session.poll();
    EXPECT_EQ(session.state(),
          SessionState::kDisconnected) << "a throwing open retries like a missing port";
}

TEST(SessionLink, TreatsAForeignThrowFromOpenAsAClosedPort)
{
    // The constructor opens eagerly, so a foreign throw escapes construction
    // instead of reading as a missing port.
    ManualClock clock;
    bool constructed = false;
    try {
    Session session(
      SessionConfig{}, clock, clock, []() -> std::unique_ptr<Transport> {throw 42;},
      [](const TopicInfo &) {return std::uint8_t{0};}, SessionEvents{});
    constructed = true;
    } catch (int) {
    }
    EXPECT_FALSE(constructed) << "a foreign throw propagates instead of reading as a missing port";
}

TEST(SessionLink, KeepsUserFramesAcrossAHandshakeRestart)
{
    Fixture fixture;
    // A port that takes five bytes of the HELLO_REQ and then blocks: the
    // walk and the user frame queue behind the half-written head, and a link
    // timeout restarts the handshake over them.
    fixture.script_write({5u, IoStatus::kOk});
    for (int i = 0; i < 12000; ++i) {
    fixture.script_write({0u, IoStatus::kWouldBlock});
    }
    fixture.advance_to(2600);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(200u, 0xAAu)));
    const std::size_t queued = fixture.session().pending_bytes();
    EXPECT_GT(queued, 0u);
    fixture.advance_to(5000);
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking)
        << "silence past the link timeout restarts the walk";
    EXPECT_LT(fixture.session().pending_bytes(),
          queued) << "the dead walk's control frames are gone";
    EXPECT_GT(fixture.session().pending_bytes(),
          0u) << "the half-written head and the user frame survive";
}

TEST(SessionLink, ScansQueuedControlsForANewSlot)
{
    Fixture fixture;
    fixture.block_writes();
    fixture.advance_to(2600);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_GT(fixture.session().pending_bytes(), 0u)
        << "the walk queues behind the blocked link without coalescing";
}

// covers: SES-35, REL-2
TEST(SessionLink, DropsAnActionGoalWhenTheTxWindowIsFull)
{
  Fixture fixture(
    [](const TopicInfo &) { return std::uint8_t{0}; }, SessionConfig(), SessionEvents(),
    ServiceResolver(), [](const ActionInfo &) { return kActionStatusOk; });
  fixture.block_writes();
  fixture.advance_to(2600);
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(3u, 0u, std::vector<std::uint8_t>{0x04u, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u,
    std::vector<std::uint8_t>{kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> action_hash{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
  fixture.deliver_frame(kHelperActions, 0u,
    helper_action_declare(0u, action_hash.data(), "test_msgs/action/Fibonacci", "spin"));
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActStatus, 0x00u, 0x01u, 0x01u});
  ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
  // One user frame leaves room: a GOAL still queues behind it.
  ASSERT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(200u, 0xAAu)));
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}))
      << "a partial window still queues the goal";
  // Fill the 515-byte MCU window behind the blocked link: two more user
  // frames stay queued because nothing flushes.
  ASSERT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(200u, 0xBBu)));
  ASSERT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(200u, 0xCCu)));
  EXPECT_GE(fixture.session().pending_bytes(), 515u) << "the window is full";
  // A GOAL while the window is full is dropped at once: the MCU could
  // not receive it, so the bridge refuses to queue it.
  EXPECT_FALSE(fixture.session().send_action_goal(0u, 0x01u, {0x05u}))
      << "a full window drops the goal instead of queuing it";
}

// covers: SES-35, REL-2
TEST(SessionLink, DropsAnActionCancelWhenTheTxWindowIsFull)
{
  Fixture fixture;
  fixture.block_writes();
  fixture.advance_to(2600);
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
  fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
  ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
  // One user frame leaves room: a CANCEL still queues behind it.
  ASSERT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(200u, 0xAAu)));
  EXPECT_TRUE(fixture.session().send_action_cancel(0x00u))
      << "a partial window still queues the cancel";
  // Fill the 515-byte MCU window behind the blocked link: two more user
  // frames stay queued because nothing flushes.
  ASSERT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(200u, 0xBBu)));
  ASSERT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(200u, 0xCCu)));
  EXPECT_GE(fixture.session().pending_bytes(), 515u) << "the window is full";
  // A CANCEL while the window is full is dropped at once: the MCU could
  // not receive it, so the bridge refuses to queue it.
  EXPECT_FALSE(fixture.session().send_action_cancel(0x01u))
      << "a full window drops the cancel instead of queuing it";
}

// The reliable channel and the parameter walk (X4): the session answers the
// walk, sends PARAM_SETs stop-and-wait, and acks every reliable frame.

constexpr std::uint8_t kParams = 3u;
constexpr std::uint8_t kAck = 6u;
constexpr std::uint8_t kParamReq = 0x01u;
constexpr std::uint8_t kParamDeclare = 0x02u;
constexpr std::uint8_t kParamSet = 0x03u;
constexpr std::uint8_t kParamStatus = 0x04u;

// A config with the heartbeat effectively off: these tests target the
// reliable exchange, and heartbeats have their own tests and vectors.
SessionConfig quiet_config()
{
  SessionConfig config;
  config.heartbeat_period_ms = 3600000;
  return config;
}

// Drains the host frames since the last drain as decoded frames, the way
// the transcripts list them.
std::vector<std::vector<std::uint8_t>> drain_decoded(Fixture & fixture)
{
  std::vector<std::vector<std::uint8_t>> out;
  for (const auto & unit : fixture.drain_units()) {
    out.push_back(decoded_of(unit));
  }
  return out;
}

// Drives one connected session with a parameter to walk: one topic and a
// bool led announced, every answer discarded. Frame seqs on channels 3 and
// 6 stay at 0: the walk's PARAM_REQs carry their own walk-local seq, and no
// SET goes out before the test triggers one. Time sync stays off, so no
// PING disturbs the captures.
void handshake_with_params(Fixture & fixture)
{
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0x04u, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u,
    {kParamDeclare, 0x00u, 0x00u, 0x00u, 0x03u, 'l', 'e', 'd', 0x01u});
  fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  drain_decoded(fixture);
}

// The host SETs of one transcript, one per rel seq: retransmits share their
// first send's rel seq, so the unique ones are the sends.
std::vector<std::vector<std::uint8_t>> reliable_sets(const std::vector<Step> & steps)
{
  std::vector<std::vector<std::uint8_t>> out;
  for (const auto & step : steps) {
    if (step.from_host && step.decoded.size() > 5u && step.decoded[0] == kParams &&
      step.decoded[2] == kParamSet)
    {
      if (out.empty() || step.decoded[3] != out.back()[3]) {
        out.push_back(step.decoded);
      }
    }
  }
  return out;
}

// Sends one transcript SET through the session: index and value travel after
// the opcode and rel seq, so they sit at bytes 4 and 5, ahead of the check.
void send_transcript_set(Fixture & fixture, const std::vector<std::uint8_t> & set)
{
  ASSERT_GE(set.size(), 7u);
  EXPECT_TRUE(fixture.session().send_param_set(set[4], {set.begin() + 5, set.end() - 2}));
}

// Replays one reliable-channel transcript after the scripted handshake: the
// SETs go out through send_param_set, so the transcript's host SET steps
// match. Only parameter and ack traffic is compared: control, time and
// heartbeat frames depend on pre-history the transcripts do not carry, and
// other transcripts cover them. When upfront is set the first SET goes out
// before the steps; otherwise it waits for the walk to connect, as a
// re-handshake does. When chain is set, the first applied STATUS sends the
// transcript's next SET, the way the parameters layer will.
void replay_reliable(const std::string & filename, bool upfront, bool chain)
{
  const std::string path = std::string(R2S_SESSION_VECTOR_DIR) + "/" + filename;
  ASSERT_TRUE(std::filesystem::exists(path)) << path << ": the transcript is missing";
  const auto [name, steps] = load_steps(path);
  ASSERT_FALSE(steps.empty()) << name;

  std::vector<std::pair<std::uint8_t, std::uint8_t>> statuses;
  SessionEvents events;
  events.param_status = [&statuses](std::uint8_t index, std::uint8_t status) {
      statuses.emplace_back(index, status);
    };
  Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, SessionConfig(), events};
  handshake_with_params(fixture);

  const std::vector<std::vector<std::uint8_t>> sets = reliable_sets(steps);
  ASSERT_FALSE(sets.empty()) << name << ": the transcript sends no SET";
  std::size_t set_next = 0u;
  bool chained = false;
  const auto trigger = [&]() {
      if (set_next == 0u && fixture.session().state() == SessionState::kConnected) {
        send_transcript_set(fixture, sets[0]);
        ++set_next;
      }
      if (chain && !chained && !statuses.empty() && statuses.back().second == 0u &&
        set_next < sets.size())
      {
        send_transcript_set(fixture, sets[set_next]);
        ++set_next;
        chained = true;
      }
    };
  if (upfront) {
    trigger();
  }

  std::vector<std::size_t> expected;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    if (steps[i].from_host && !steps[i].decoded.empty() &&
      (steps[i].decoded[0] == kParams || steps[i].decoded[0] == kAck))
    {
      expected.push_back(i);
    }
  }
    // Compares one decoded unit with the transcript's host steps, skipping
    // every other channel: the decoded unit starts with the topic id,
    // where a wire unit would start with an rCOBS distance code.
  std::size_t next = 0;
  const auto check = [&](const std::vector<std::uint8_t> & unit) {
      if (unit.size() < 3u || (unit[0] != kParams && unit[0] != kAck)) {
        return;
      }
      ASSERT_LT(next, expected.size()) << name << ": the session wrote more than the transcript";
      EXPECT_EQ(unit, steps[expected[next]].decoded) << name << " host step " << expected[next];
      ++next;
    };
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const Step & step = steps[i];
    SCOPED_TRACE(name + " step " + std::to_string(i));
    fixture.advance_to(step.at_ms);
    if (!step.from_host) {
      fixture.deliver(step.decoded);
    }
    trigger();
    for (const auto & unit : drain_decoded(fixture)) {
      check(unit);
    }
  }
  if (next < expected.size()) {
    fixture.advance_to(steps.back().at_ms + 500);
    for (const auto & unit : drain_decoded(fixture)) {
      check(unit);
    }
  }
  EXPECT_EQ(next,
          expected.size()) << name << ": the transcript's reliable frames did not all go out";
}

// covers: REL-1, REL-2, REL-3, REL-4, PAR-7, PAR-8
TEST(ReliableReplay, ReplaysParamSetReliable)
{
    replay_reliable("param_set_reliable.yaml", true, true);
}

// covers: REL-2, HB-7
TEST(ReliableReplay, ReplaysParamSetGiveup)
{
    replay_reliable("param_set_giveup.yaml", true, false);
}

// covers: REL-5, PAR-8
TEST(ReliableReplay, ReplaysParamRehandshake)
{
    replay_reliable("param_rehandshake.yaml", false, false);
}

// covers: PAR-7
TEST(ReliableReplay, ReplaysParamWalk)
{
    SessionConfig config;
    config.time_sync_enabled = false;
    replay_transcript(std::string(R2S_SESSION_VECTOR_DIR) + "/param_walk.yaml", config);
}

// covers: PAR-7
TEST(ReliableReplay, ReplaysParamWalkRetry)
{
    SessionConfig config;
    config.time_sync_enabled = false;
    replay_transcript(std::string(R2S_SESSION_VECTOR_DIR) + "/param_walk_retry.yaml", config);
}

// covers: PAR-7
TEST(ReliableWalk, StartsAfterTheTopicWalk)
{
    Fixture fixture;
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 3u);
    EXPECT_EQ(units[1][2], 0x05u) << "the topic walk answers first";
    EXPECT_EQ(units[2][0], kParams) << "then the parameter walk starts";
    EXPECT_EQ(units[2][2], kParamReq) << "with a PARAM_REQ for index 0";
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
}

// covers: CTL-17
TEST(ReliableWalk, RefusesServicesWithoutTheReliableBit)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 4u, 0x40u, 1000u));
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().seen_hello());
}

// covers: CTL-2, CTL-3
TEST(ReliableWalk, AcceptsAFeaturesByteWithServicesAndActions)
{
    Fixture fixture;
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 4u, 0xFFu, 1000u));
    // Bits 6 and 7 are services and actions, so no bit is reserved anymore:
    // every features byte, this one included, is accepted and parsed.
    const auto units = drain_decoded(fixture);
    ASSERT_FALSE(units.empty());
    EXPECT_EQ(units[0][2], kParamReq) << "the parameter walk starts: the HELLO was accepted";
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
}

// covers: PAR-7
TEST(ReliableWalk, SkipsWhenTheMcuAnnouncesNoParameters)
{
    // Time sync stays off: features announce time, and the first PING
    // would follow READY in the same poll.
    SessionConfig config;
    config.time_sync_enabled = false;
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, config};
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x01u, 1000u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 3u);
    EXPECT_EQ(units[2][2], 0x06u) << "READY goes out with no parameter walk";
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: PAR-1, PAR-7
TEST(ReliableWalk, CollectsDeclaresForTheParametersLayer)
{
    Fixture fixture;
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    fixture.deliver_frame(kParams, 0u,
    {kParamDeclare, 0x00u, 0x00u, 0x00u, 0x03u, 'l', 'e', 'd', 0x01u});
    fixture.deliver_frame(kParams, 1u,
    {kParamDeclare, 0x01u, 0x01u, 0x00u, 0x02u, 'h', 'z', 0x02u, 0x00u, 0x00u, 0x00u,
      0x00u, 0x00u, 0x00u, 0x00u});
    fixture.deliver_frame(kParams, 2u, {kParamStatus, 0x00u, 0x02u, 0x01u});
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    const std::vector<ParamInfo> & params = fixture.session().params();
    ASSERT_EQ(params.size(), 2u);
    EXPECT_EQ(params[0].index, 0u);
    EXPECT_EQ(params[0].type, 0u);
    EXPECT_EQ(params[0].flags, 0u);
    EXPECT_EQ(params[0].name, "led");
    EXPECT_EQ(params[0].default_value, std::vector<std::uint8_t>{0x01u});
    EXPECT_EQ(params[1].index, 1u);
    EXPECT_EQ(params[1].type, 1u);
    EXPECT_EQ(params[1].flags, 0u);
    EXPECT_EQ(params[1].name, "hz");
    EXPECT_EQ(params[1].default_value,
              std::vector<std::uint8_t>({0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}));
}

// covers: PAR-1, PAR-3, PAR-7
TEST(ReliableWalk, SkipsUnparseableDeclares)
{
    // Every broken shape still moves the walk on: only the valid DECLARE
    // is kept for the parameters layer.
    Fixture fixture;
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    fixture.deliver_frame(kParams, 0u, {kParamDeclare, 0x00u});
    fixture.deliver_frame(kParams, 1u, {kParamDeclare, 0x01u, 0x04u, 0x00u, 0x00u});
    fixture.deliver_frame(kParams, 2u, {kParamDeclare, 0x02u, 0x00u, 0x02u, 0x00u});
    fixture.deliver_frame(kParams, 3u, {kParamDeclare, 0x03u, 0x00u, 0x00u, 0x05u, 'a', 'b'});
    fixture.deliver_frame(kParams, 4u,
    {kParamDeclare, 0x04u, 0x00u, 0x00u, 0x01u, 'c', 0x02u, 0x03u});
    fixture.deliver_frame(kParams, 5u,
    {kParamDeclare, 0x05u, 0x03u, 0x00u, 0x01u, 'x', 0x01u, 0x00u});
    fixture.deliver_frame(kParams, 6u,
    {kParamDeclare, 0x06u, 0x01u, 0x00u, 0x01u, 'y', 0x02u, 0x00u});
    fixture.deliver_frame(kParams, 7u,
    {kParamDeclare, 0x07u, 0x00u, 0x00u, 0x02u, 'o', 'k', 0x00u});
    fixture.deliver_frame(kParams, 8u, {kParamDeclare, 0x08u, 0x00u});
    fixture.deliver_frame(kParams, 9u,
    {kParamDeclare, 0x09u, 0x03u, 0x00u, 0x01u, 't', 0x05u, 'a'});
    fixture.deliver_frame(kParams, 10u,
    {kParamDeclare, 0x0Au, 0x03u, 0x00u, 0x01u, 'u', 0x01u, 'z', 0xFFu});
    fixture.deliver_frame(kParams, 11u, {kParamDeclare, 0x0Bu, 0x00u, 0x00u, 0x01u, 'v', 0x02u});
    fixture.deliver_frame(kParams, 12u, {kParamStatus, 0x00u, 0x0Cu, 0x01u});
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    const std::vector<ParamInfo> & params = fixture.session().params();
    ASSERT_EQ(params.size(), 1u);
    EXPECT_EQ(params[0].name, "ok");
}

// covers: PAR-7
TEST(ReliableWalk, GivesUpAfterThreeRetries)
{
    Fixture fixture;
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    EXPECT_EQ(drain_decoded(fixture).size(), 3u);
    for (int retry = 0; retry < 3; ++retry) {
    fixture.advance_to(200u * static_cast<std::uint32_t>(retry + 1u));
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "retry " << retry;
    EXPECT_EQ(units[0][2], kParamReq) << "the walk retries the REQ";
    }
    fixture.advance_to(800);
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "after the third retry the handshake starts over";
    EXPECT_EQ(units[0][2], 0x01u) << "a HELLO_REQ, not another PARAM_REQ";
}

// covers: PAR-7
TEST(ReliableWalk, IgnoresWalkFramesForAnotherIndex)
{
    Fixture fixture;
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    drain_decoded(fixture);
    fixture.deliver_frame(kParams, 0u, {kParamDeclare, 0x05u});
    fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x05u, 0x00u});
    fixture.deliver_frame(kParams, 2u, {kParamReq});
    fixture.deliver_frame(kParams, 3u, {0x09u});
    fixture.deliver_frame(kParams, 4u, {kParamDeclare});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "nothing answers a frame for another index";
    fixture.advance_to(200);
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "the walk still waits on index 0";
    EXPECT_EQ(units[0], with_check({kParams, 1u, kParamReq, 0x00u}));
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
}

// covers: PAR-7
TEST(ReliableWalk, AdvancesOnAnAppliedStatus)
{
    Fixture fixture;
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    drain_decoded(fixture);
    fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(units[0], with_check({kParams, 1u, kParamReq, 0x01u}))
        << "an applied STATUS walks to the next index";
}

// covers: REL-7
TEST(ReliableWalk, CoalescesParamReqBehindABlockedLink)
{
    Fixture fixture;
    fixture.block_writes();
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    EXPECT_EQ(fixture.session().control_coalesced(), 0u) << "nothing replaced yet";
    fixture.advance_to(200);
    fixture.advance_to(400);
    EXPECT_EQ(fixture.session().control_coalesced(), 2u) << "each retry replaces in place";
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 3u);
    EXPECT_EQ(units[2],
          with_check({kParams, 0u, kParamReq, 0x00u})) << "the REQ keeps the first seq";
}

// covers: REL-2
TEST(ReliableSend, RetransmitsAndStopsOnAck)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(units[0], with_check({kParams, 0u, kParamSet, 0x00u, 0x00u, 0x00u}))
        << "the SET carries rel seq 0";
    fixture.advance_to(499);
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "no retry before 500 ms";
    fixture.advance_to(500);
    units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(units[0], with_check({kParams, 1u, kParamSet, 0x00u, 0x00u, 0x00u}))
        << "the retry keeps rel seq 0 with the next frame seq";
    fixture.deliver_frame(kAck, 0u, {kParams, 0x00u});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "the ack clears the outstanding SET";
    fixture.advance_to(2500);
    for (const auto & unit : drain_decoded(fixture)) {
    EXPECT_NE(unit[0], kParams) << "no further SET goes out";
    }
}

// covers: REL-2
TEST(ReliableSend, GivesUpAfterThreeRetries)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    drain_decoded(fixture);
    for (int retry = 0; retry < 3; ++retry) {
    fixture.advance_to(500u * static_cast<std::uint32_t>(retry + 1u));
    EXPECT_EQ(drain_decoded(fixture).size(), 1u) << "retry " << retry;
    }
    // The link stays alive past the drop deadline: without MCU traffic the
    // link timeout at 2000 ms would re-handshake first.
    fixture.deliver_frame(kHeartbeat, 0u, {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u});
    fixture.advance_to(2500);
    for (const auto & unit : drain_decoded(fixture)) {
    EXPECT_NE(unit[0], kParams) << "the spent SET never goes out again";
    }
    EXPECT_EQ(fixture.session().reliable_dropped(), 1u);
}

// covers: REL-1, REL-3, REL-4
TEST(ReliableReceive, AcksEveryStatusAndAppliesOnce)
{
    std::vector<std::pair<std::uint8_t, std::uint8_t>> statuses;
    SessionEvents events;
    events.param_status = [&statuses](std::uint8_t index, std::uint8_t status) {
      statuses.emplace_back(index, status);
    };
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, SessionConfig(), events};
    handshake_with_params(fixture);
    fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(units[0], with_check({kAck, 0u, kParams, 0x00u})) << "the ack names channel and rel";
    ASSERT_EQ(statuses.size(), 1u);
    EXPECT_EQ(statuses[0], std::make_pair(std::uint8_t{0}, std::uint8_t{0}));
    fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "the duplicate is acked again";
    EXPECT_EQ(units[0][0], kAck);
    EXPECT_EQ(statuses.size(), 1u) << "but applied once";
    fixture.deliver_frame(kParams, 2u, {kParamStatus, 0x05u, 0x00u, 0x00u});
    units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "a future rel is acked too";
    EXPECT_EQ(units[0], with_check({kAck, 2u, kParams, 0x05u}));
    EXPECT_EQ(statuses.size(), 1u) << "and not applied";
}

// covers: REL-3
TEST(ReliableReceive, IgnoresMalformedStatuses)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u});
    fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x00u, 0x05u});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "short frames and bad verdicts earn no ack";
    fixture.deliver_frame(kParams, 2u, {0x09u});
    fixture.deliver_frame(kParams, 3u, {kParamDeclare, 0x00u, 0x00u, 0x00u, 0x00u});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "unknown opcodes and walk frames earn no ack";
}

// covers: REL-3
TEST(ReliableReceive, AcksWithoutAnEventSink)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    fixture.deliver_frame(kParams, 0u, {kParamDeclare, 0x00u, 0x00u, 0x00u, 0x00u});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "a DECLARE is never acked";
    fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(units[0], with_check({kAck, 0u, kParams, 0x00u}));
}

// covers: REL-6
TEST(ReliableReceive, IgnoresReliableFramesBeforeTheHandshake)
{
    Fixture fixture;
    fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    fixture.deliver_frame(kAck, 0u, {kParams, 0x00u});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "nothing answers before the handshake";
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "the session still walks after stray frames";
    EXPECT_EQ(units[0][2], 0x03u) << "a TOPIC_REQ";
}

// covers: REL-2
TEST(ReliableAck, IgnoresMismatchedAcks)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    drain_decoded(fixture);
    fixture.deliver_frame(kAck, 0u, {0x04u, 0x00u});
    fixture.deliver_frame(kAck, 1u, {kParams, 0x09u});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "neither ack names the outstanding SET";
    fixture.advance_to(500);
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "the outstanding SET still retries";
    EXPECT_EQ(units[0], with_check({kParams, 1u, kParamSet, 0x00u, 0x00u, 0x00u}));
    fixture.deliver_frame(kAck, 2u, {kParams, 0x00u});
    fixture.deliver_frame(kAck, 3u, {kParams, 0x00u});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "the second ack answers nothing";
    EXPECT_EQ(fixture.session().reliable_dropped(), 0u);
}

// covers: REL-6
TEST(ReliableAck, CoalescesBehindABlockedLink)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    fixture.block_writes();
    fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    fixture.deliver_frame(kParams, 2u, {kParamStatus, 0x01u, 0x01u, 0x01u});
    EXPECT_EQ(fixture.session().control_coalesced(), 2u) << "each replaced ack counts";
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "one ack goes out for the whole backlog";
    EXPECT_EQ(units[0][0], kAck);
    EXPECT_EQ(units[0][3], 0x01u) << "carrying the newest rel seq";
}

// covers: REL-2
TEST(ReliableSend, RefusesToSendWhileNotConnected)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    EXPECT_FALSE(fixture.session().send_param_set(0u,
    {0x00u})) << "no SET leaves a boot-waiting session";
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    EXPECT_FALSE(fixture.session().send_param_set(0u,
    {0x00u})) << "no SET leaves a handshaking session";
}

// covers: REL-7
TEST(ReliableSend, RefusesToSendWhenTheLinkIsFull)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    fixture.block_writes();
    EXPECT_TRUE(fixture.session().send_user(16u, std::vector<std::uint8_t>(400u, 0xABu)));
    EXPECT_TRUE(fixture.session().send_user(16u, std::vector<std::uint8_t>(200u, 0xABu)));
    EXPECT_FALSE(fixture.session().send_param_set(0u, {0x00u}))
        << "a queued SET would overflow the link window";
}

// covers: REL-7
TEST(ReliableSend, QueuesOnePendingSetPerIndex)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    EXPECT_TRUE(fixture.session().send_param_set(1u, {0x07u}));
    EXPECT_EQ(fixture.session().reliable_replaced(),
          0u) << "a new index pends beside the outstanding one";
    drain_decoded(fixture);
    fixture.deliver_frame(kAck, 0u, {kParams, 0x00u});
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "the pending SET goes out with the next rel seq";
    EXPECT_EQ(units[0], with_check({kParams, 1u, kParamSet, 0x01u, 0x01u, 0x07u}));
}

// covers: REL-2
TEST(ReliableSend, HoldsTheRetryWhileTheLinkIsBlocked)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    fixture.block_writes();
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    fixture.advance_to(1500);
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "deadlines pass without spending a retry";
    EXPECT_EQ(fixture.session().reliable_dropped(), 0u);
    fixture.deliver_frame(kHeartbeat, 0u, {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u});
    fixture.allow_writes();
    fixture.session().poll();
    const auto sent = drain_decoded(fixture);
    ASSERT_EQ(sent.size(), 2u) << "the queued SET and its overdue retry go out at once";
    EXPECT_EQ(sent[0], with_check({kParams, 0u, kParamSet, 0x00u, 0x00u, 0x00u}));
    EXPECT_EQ(sent[1], with_check({kParams, 1u, kParamSet, 0x00u, 0x00u, 0x00u}));
    fixture.advance_to(2400);
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "the next retry keeps its 500 ms cadence";
    EXPECT_EQ(units[0], with_check({kParams, 2u, kParamSet, 0x00u, 0x00u, 0x00u}));
    EXPECT_EQ(fixture.session().reliable_dropped(), 0u);
}

// covers: REL-2, REL-5
TEST(ReliableSend, DropsQueuedSetsOnLinkTimeout)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    fixture.block_writes();
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    fixture.advance_to(2500);
    EXPECT_EQ(fixture.session().reliable_dropped(), 0u) << "a timeout drops, it never gives up";
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = drain_decoded(fixture);
    ASSERT_FALSE(units.empty());
    for (const auto & unit : units) {
    EXPECT_NE(unit[0], kParams) << "no SET survives the re-handshake";
    }
    EXPECT_EQ(units.back()[2], 0x01u) << "the handshake starts over";
}

// covers: REL-7
TEST(ReliableSend, ReplacesAPendingSetForTheSameIndex)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x01u}));
    EXPECT_EQ(fixture.session().reliable_replaced(), 0u) << "the first pending SET only queues";
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x02u}));
    EXPECT_EQ(fixture.session().reliable_replaced(), 1u) << "the second replaces it in place";
    drain_decoded(fixture);
    fixture.deliver_frame(kAck, 0u, {kParams, 0x00u});
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "the replacement goes out after the ack";
    EXPECT_EQ(units[0], with_check({kParams, 1u, kParamSet, 0x01u, 0x00u, 0x02u}))
        << "with the newest value and the next rel seq";
}

// covers: PAR-7
TEST(ReliableWalk, IgnoresWalkFramesDuringTheTopicWalk)
{
    Fixture fixture;
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    fixture.deliver_frame(kParams, 0u,
    {kParamDeclare, 0x00u, 0x00u, 0x00u, 0x03u, 'l', 'e', 'd', 0x01u});
    fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "only the HELLO_REQ goes out before the topics";
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 2u);
    EXPECT_EQ(units[1][0], kParams);
    EXPECT_EQ(units[1][2], kParamReq) << "the parameter walk still starts";
    EXPECT_EQ(units[1][3], 0x00u) << "the stray STATUS never advanced it past index 0";
}

// covers: REL-3
TEST(ReliableReceive, IgnoresAnEmptyParamsPayload)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    fixture.deliver_frame(kParams, 0u, {});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "an empty params frame earns no ack";
    fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "the session still acks after the empty frame";
    EXPECT_EQ(units[0], with_check({kAck, 0u, kParams, 0x00u}));
}

// covers: REL-2
TEST(ReliableAck, IgnoresAWrongLengthAck)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    drain_decoded(fixture);
    fixture.deliver_frame(kAck, 0u, {kParams});
    fixture.deliver_frame(kAck, 1u, {kParams, 0x00u, 0xFFu});
    EXPECT_TRUE(drain_decoded(fixture).empty()) << "a short or long ack answers nothing";
    fixture.advance_to(500);
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "the outstanding SET still retries";
    EXPECT_EQ(units[0], with_check({kParams, 1u, kParamSet, 0x00u, 0x00u, 0x00u}));
}

// covers: REL-7
TEST(ReliableSend, SendsWhileUserFramesAreQueued)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    fixture.block_writes();
    EXPECT_TRUE(fixture.session().send_user(16u, std::vector<std::uint8_t>(100u, 0xABu)));
    EXPECT_TRUE(fixture.session().send_param_set(0u,
    {0x00u})) << "a SET fits beside queued user frames";
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 2u);
    EXPECT_EQ(units[0][0], 16u) << "the user frame keeps its place ahead of the SET";
    EXPECT_EQ(units[1], with_check({kParams, 0u, kParamSet, 0x00u, 0x00u, 0x00u}));
}

// covers: REL-5
TEST(ReliableSend, DropsAPartialSetOnLinkTimeout)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    // Two bytes of the SET go out, then the link blocks with the SET
    // partial at the front: the timeout drops it instead of finishing it.
    fixture.script_write({2u, IoStatus::kOk});
    fixture.block_writes();
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    fixture.advance_to(2500);
    EXPECT_EQ(fixture.session().state(),
          SessionState::kHandshaking) << "the silent link re-handshakes";
    EXPECT_EQ(fixture.session().reliable_dropped(), 0u) << "a timeout drops, it never gives up";
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 2u);
    EXPECT_EQ(units[0].size(), 3u) << "two stale bytes closed by the delimiter";
    EXPECT_EQ(units[0].back(), 0x00u);
    const auto hello = decoded_of(units[1]);
    EXPECT_EQ(hello[0], kControl) << "no SET survives the re-handshake";
    EXPECT_EQ(hello[2], 0x01u) << "the handshake starts over on a clean frame";
}

// covers: REL-5
TEST(ReliableSend, DropsTwoQueuedSetsOnLinkTimeout)
{
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
    handshake_with_params(fixture);
    fixture.block_writes();
    EXPECT_TRUE(fixture.session().send_param_set(0u, {0x00u}));
    EXPECT_TRUE(fixture.session().send_param_set(1u, {0x07u}));
    // The ack promotes the pending SET behind the outstanding frame: two
    // SET frames queue, and the timeout drops both.
    fixture.deliver_frame(kAck, 0u, {kParams, 0x00u});
    fixture.advance_to(2500);
    EXPECT_EQ(fixture.session().state(),
          SessionState::kHandshaking) << "the silent link re-handshakes";
    EXPECT_EQ(fixture.session().reliable_dropped(), 0u) << "a timeout drops, it never gives up";
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = drain_decoded(fixture);
    ASSERT_FALSE(units.empty());
    for (const auto & unit : units) {
    EXPECT_NE(unit[0], kParams) << "no SET survives the re-handshake";
    }
    EXPECT_EQ(units.back()[2], 0x01u) << "the handshake starts over";
}

// covers: REL-5
TEST(ReliableWalk, KeepsAPartialReqAcrossTheLinkTimeout)
{
    SessionConfig config;
    config.heartbeat_period_ms = 3600000;
    config.time_sync_enabled = false;
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, config};
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
    drain_decoded(fixture);
    // The topic answer flushes whole, the walk REQ wedges after two bytes,
    // and every later write blocks: the REQ stays partial at the front.
    fixture.script_write({1000u, IoStatus::kOk});
    fixture.script_write({2u, IoStatus::kOk});
    fixture.script_write({0u, IoStatus::kWouldBlock});
    fixture.block_writes();
    fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
    fixture.advance_to(2500);
    EXPECT_EQ(fixture.session().state(),
          SessionState::kHandshaking) << "the silent link re-handshakes";
    EXPECT_EQ(fixture.session().reliable_dropped(), 0u) << "a REQ is not a SET";
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 3u);
    EXPECT_EQ(units[0], with_check({kControl, 0x01u, 0x05u, 0x10u, 0x00u}))
        << "the topic answer flushed before the timeout";
    EXPECT_EQ(units[1], with_check({kParams, 0u, kParamReq, 0x00u})) << "the partial REQ completes";
    EXPECT_EQ(units[2][2], 0x01u) << "and the handshake starts over";
}

// covers: REL-5
TEST(ReliableSend, KeepsAPartialAckAcrossTheLinkTimeout)
{
    SessionConfig config;
    config.heartbeat_period_ms = 3600000;
    config.time_sync_enabled = false;
    Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, config};
    handshake_with_params(fixture);
    fixture.script_write({2u, IoStatus::kOk});
    fixture.block_writes();
    fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x00u});
    fixture.advance_to(2500);
    EXPECT_EQ(fixture.session().state(),
          SessionState::kHandshaking) << "the silent link re-handshakes";
    EXPECT_EQ(fixture.session().reliable_dropped(), 0u) << "an ack is not a SET";
    fixture.allow_writes();
    fixture.session().poll();
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 2u);
    EXPECT_EQ(units[0], with_check({kAck, 0u, kParams, 0x00u})) << "the partial ack completes";
    EXPECT_EQ(units[1][2], 0x01u) << "and the handshake starts over";
}

// The services channel: the walk after the parameters, and the REQUEST and
// RESPONSE exchange on the reliable channel (spec/protocol.md, 7.8).
// covers: SVR-1, SVR-2, SVR-7

// One service DECLARE for the walk tests: the resolver answers ok to every
// DECLARE, so the hash is a placeholder.
std::vector<std::uint8_t> svc_declare(
  std::uint8_t index, std::uint8_t role, const std::string & name)
{
  const std::array<std::uint8_t, 8> hash{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
  return helper_service_declare(index, role, hash.data(), "std_srvs/srv/Trigger", name);
}

// A connected session with services: one topic, no parameters, one served
// Trigger "reset" and one called Trigger "trigger". Time sync stays off and
// the heartbeat is quiet, so only service traffic is captured.
void handshake_with_services(Fixture & fixture)
{
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 1u, svc_declare(1u, 0x01u, "trigger"));
  fixture.deliver_frame(kHelperServices, 2u, {kHelperSvcStatus, 0x00u, 0x02u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  drain_decoded(fixture);
}

// covers: SVR-9
TEST(ServiceReplay, ReplaysServiceNotConnected)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  replay_transcript(
    std::string(R2S_SESSION_VECTOR_DIR) + "/service_not_connected.yaml", config,
    [](const ServiceInfo &) {return kServiceStatusOk;});
}

// covers: SVR-7
TEST(ServiceReplay, ReplaysServiceWalk)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  replay_transcript(
    std::string(R2S_SESSION_VECTOR_DIR) + "/service_walk.yaml", config,
    [](const ServiceInfo &) {return kServiceStatusOk;});
}

// Replays one service-call transcript after the scripted handshake: the
// transcript's host REQUEST goes out through send_service_request once
// connected, and its host RESPONSE through send_service_response once the
// MCU REQUEST arrives, the way the router will drive them. Only services
// and ack traffic is compared: control, time and heartbeat frames depend
// on pre-history the transcripts do not carry, and other transcripts
// cover them.
// covers: SVR-4, SVR-5, SVR-6, REL-3
TEST(ServiceReplay, ReplaysServiceCall)
{
  const std::string path =
    std::string(R2S_SESSION_VECTOR_DIR) + "/service_call.yaml";
  ASSERT_TRUE(std::filesystem::exists(path)) << path << ": the transcript is missing";
  const auto [name, steps] = load_steps(path);
  ASSERT_FALSE(steps.empty()) << name;

  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> calls;
  SessionEvents events;
  events.service_request =
    [&calls](std::uint8_t index, std::uint8_t req, const std::vector<std::uint8_t> & payload) {
      calls.emplace_back(index, req, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, SessionConfig(), std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);

  std::vector<std::vector<std::uint8_t>> calls_out;
  for (const auto & step : steps) {
    if (step.from_host && step.decoded.size() > 7u &&
      step.decoded[0] == kHelperServices &&
      (step.decoded[2] == kHelperSvcRequest || step.decoded[2] == kHelperSvcResponse))
    {
      calls_out.push_back(step.decoded);
    }
  }
  ASSERT_EQ(calls_out.size(), 2u) << name << ": one REQUEST and one RESPONSE drive the call";
  ASSERT_EQ(calls_out[0][2], kHelperSvcRequest);
  ASSERT_EQ(calls_out[1][2], kHelperSvcResponse);

  EXPECT_TRUE(fixture.session().send_service_request(calls_out[0][5], calls_out[0][4],
    {calls_out[0].begin() + 6, calls_out[0].end() - 2}));
  bool answered = false;
  std::vector<std::size_t> expected;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    if (steps[i].from_host && steps[i].decoded.size() > 2u &&
      (steps[i].decoded[0] == kHelperServices || steps[i].decoded[0] == kAck))
    {
      expected.push_back(i);
    }
  }
  std::size_t next = 0u;
  const auto check = [&](const std::vector<std::uint8_t> & unit) {
      if (unit.size() < 3u || (unit[0] != kHelperServices && unit[0] != kAck)) {
        return;
      }
      ASSERT_LT(next, expected.size()) << name << ": the session wrote more than the transcript";
      EXPECT_EQ(unit, steps[expected[next]].decoded) << name << " host step " << expected[next];
      ++next;
    };
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const Step & step = steps[i];
    SCOPED_TRACE(name + " step " + std::to_string(i));
    fixture.advance_to(step.at_ms);
    if (!step.from_host) {
      fixture.deliver(step.decoded);
    }
    if (!answered && !calls.empty()) {
      EXPECT_TRUE(fixture.session().send_service_response(calls_out[1][4], calls_out[1][5],
        {calls_out[1].begin() + 6, calls_out[1].end() - 2}));
      answered = true;
    }
    for (const auto & unit : drain_decoded(fixture)) {
      check(unit);
    }
  }
  if (next < expected.size()) {
    fixture.advance_to(steps.back().at_ms + 500);
    for (const auto & unit : drain_decoded(fixture)) {
      check(unit);
    }
  }
  EXPECT_EQ(next,
          expected.size()) << name << ": the transcript's service frames did not all go out";
}

// The actions channel: the walk after the services, and the GOAL, CANCEL,
// FEEDBACK and RESULT exchange on the reliable channel (spec/protocol.md,
// 7.9).
// covers: ACT-1, ACT-2, ACT-7

// One action DECLARE for the walk tests: the resolver answers ok to every
// DECLARE, so the hash is a placeholder.
std::vector<std::uint8_t> act_declare(std::uint8_t index, const std::string & name)
{
  const std::array<std::uint8_t, 8> hash{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
  return helper_action_declare(index, hash.data(), "test_msgs/action/Fibonacci", name);
}

// A connected session with services and actions: one topic, no parameters,
// no services, one action "spin". Time sync stays off and the heartbeat is
// quiet, so only action traffic is captured.
void handshake_with_actions(Fixture & fixture)
{
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperActions, 0u, act_declare(0u, "spin"));
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  drain_decoded(fixture);
}

// A QUERY with no index byte is malformed: it names nothing and earns no
// answer, instead of reading past the payload.
// covers: ACT-1
TEST(ActionExchange, DropsAQueryWithNoIndexByte)
{
  Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
  handshake_with_actions(fixture);
  drain_decoded(fixture);
  // Topic 8 is the wire actions topic; 0x01 is the QUERY opcode.
  fixture.deliver_frame(8u, 0u, {0x01u});
  EXPECT_TRUE(drain_decoded(fixture).empty()) << "an index-less QUERY earns no answer";
}

// A DECLARE whose hash does not match the installed action is refused with
// a hash-mismatch verdict, the way a stale MCU layout is.
// covers: ACT-7
TEST(ActionExchange, RefusesAnActionDeclareWithABadHash)
{
  Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, quiet_config()};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  const std::array<std::uint8_t, 8> bad_hash{9u, 9u, 9u, 9u, 9u, 9u, 9u, 9u};
  fixture.deliver_frame(kHelperActions, 0u,
    helper_action_declare(0u, bad_hash.data(), "test_msgs/action/Fibonacci", "spin"));
  const auto units = drain_decoded(fixture);
  ASSERT_FALSE(units.empty()) << "the refusal answers the DECLARE";
  EXPECT_EQ(units[0][2], kHelperActStatus) << "the answer is a status verdict";
  EXPECT_EQ(units[0][5], kActionStatusHashMismatch) << "the verdict names the hash mismatch";
}

// An unacked GOAL is retried then dropped: the drop forgets that wire goal,
// so a late answer for it is stale and ignored.
// covers: ACT-4
TEST(ActionExchange, DropsAnUnackedGoalAfterRetries)
{
  SessionConfig config = quiet_config();
  config.link_timeout_ms = 3600000;
  config.time_sync_enabled = false;
  Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, config};
  handshake_with_actions(fixture);
  drain_decoded(fixture);
  ASSERT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x01u}));
  drain_decoded(fixture);
  fixture.advance_to(3000);
  EXPECT_EQ(fixture.session().reliable_dropped(), 1u) << "the unacked GOAL is dropped";
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// An unacked CANCEL is retried then dropped the same way: the drop forgets
// that wire cancel.
// covers: ACT-4
TEST(ActionExchange, DropsAnUnackedCancelAfterRetriesExhausted)
{
  SessionConfig config = quiet_config();
  config.link_timeout_ms = 3600000;
  config.time_sync_enabled = false;
  Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, config};
  handshake_with_actions(fixture);
  drain_decoded(fixture);
  ASSERT_TRUE(fixture.session().send_action_cancel(0x00u));
  drain_decoded(fixture);
  fixture.advance_to(3000);
  EXPECT_EQ(fixture.session().reliable_dropped(), 1u) << "the unacked CANCEL is dropped";
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: ACT-7
TEST(ActionReplay, ReplaysActionWalk)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  replay_transcript(
    std::string(R2S_SESSION_VECTOR_DIR) + "/action_walk.yaml", config,
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;});
}

// Replays one action transcript after the scripted handshake: the
// transcript's host GOAL and CANCEL go out through send_action_goal and
// send_action_cancel, the way the router will drive them. Only action and
// ack traffic is compared: control, time and heartbeat frames depend on
// pre-history the transcripts do not carry, and other transcripts cover
// them.
// covers: ACT-2, ACT-3, ACT-8
void replay_action_exchange(const std::string & file)
{
  const std::string path = std::string(R2S_SESSION_VECTOR_DIR) + "/" + file;
  ASSERT_TRUE(std::filesystem::exists(path)) << path << ": the transcript is missing";
  const auto [name, steps] = load_steps(path);
  ASSERT_FALSE(steps.empty()) << name;

  SessionEvents events;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, SessionConfig(), std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};

  std::vector<std::size_t> expected;
  for (std::size_t i = 0; i < steps.size(); ++i) {
    if (steps[i].from_host && steps[i].decoded.size() > 2u &&
      (steps[i].decoded[0] == kHelperActions || steps[i].decoded[0] == kAck))
    {
      expected.push_back(i);
    }
  }
  ASSERT_FALSE(expected.empty()) << name << ": no action or ack step drives the exchange";
  std::size_t next = 0u;
  const auto check = [&](const std::vector<std::uint8_t> & unit) {
      if (unit.size() < 3u || (unit[0] != kHelperActions && unit[0] != kAck)) {
        return;
      }
      ASSERT_LT(next, expected.size()) << name << ": the session wrote more than the transcript";
      EXPECT_EQ(unit, steps[expected[next]].decoded) << name << " host step " << expected[next];
      ++next;
    };
  for (std::size_t i = 0; i < steps.size(); ++i) {
    const Step & step = steps[i];
    SCOPED_TRACE(name + " step " + std::to_string(i));
    fixture.advance_to(step.at_ms);
    const bool is_goal = step.decoded.size() > 7u && step.decoded[0] == kHelperActions &&
      step.decoded[2] == kHelperActGoal;
    const bool is_cancel = step.decoded.size() > 5u && step.decoded[0] == kHelperActions &&
      step.decoded[2] == kHelperActCancel;
    if (!step.from_host) {
      fixture.deliver(step.decoded);
    } else if (is_goal) {
      EXPECT_TRUE(fixture.session().send_action_goal(step.decoded[5], step.decoded[4],
        {step.decoded.begin() + 6, step.decoded.end() - 2}));
    } else if (is_cancel) {
      EXPECT_TRUE(fixture.session().send_action_cancel(step.decoded[4]));
    }
    for (const auto & unit : drain_decoded(fixture)) {
      check(unit);
    }
  }
  if (next < expected.size()) {
    fixture.advance_to(steps.back().at_ms + 500);
    for (const auto & unit : drain_decoded(fixture)) {
      check(unit);
    }
  }
  EXPECT_EQ(next, expected.size()) << name << ": the transcript's frames did not all go out";
}

// covers: ACT-2, ACT-3, ACT-8
TEST(ActionReplay, ReplaysActionGoal)
{
  replay_action_exchange("action_goal.yaml");
}

// covers: ACT-5, ACT-8
TEST(ActionReplay, ReplaysActionCancel)
{
  replay_action_exchange("action_cancel.yaml");
}

// covers: ACT-4, ACT-8
TEST(ActionReplay, ReplaysActionSecondGoal)
{
  replay_action_exchange("action_second_goal.yaml");
}

// covers: ACT-8, ACT-9
TEST(ActionReplay, ReplaysActionResetAbort)
{
  replay_action_exchange("action_reset_abort.yaml");
}

// covers: ACT-7
TEST(ActionWalk, StartsAfterTheServiceWalk)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  drain_decoded(fixture);
  fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(units[0][0], kHelperActions);
  EXPECT_EQ(units[0][2], kHelperActQuery) << "the action walk starts with index 0";
  EXPECT_EQ(units[0][3], 0x00u);
  EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
}

// covers: ACT-7
TEST(ActionWalk, SkipsWhenTheMcuAnnouncesNoActions)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  const auto units = drain_decoded(fixture);
  ASSERT_FALSE(units.empty());
  EXPECT_EQ(units.back()[2], 0x06u) << "READY goes out with no action walk";
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  EXPECT_TRUE(fixture.session().actions().empty());
}

// covers: ACT-7
TEST(ActionWalk, RecordsVerdicts)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo & info) {
      if (info.action_name == "spin") {
        return kActionStatusOk;
      }
      if (info.action_name == "old") {
        return kActionStatusHashMismatch;
      }
      return kActionStatusUnknownType;
    }};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 0u, act_declare(0u, "spin"));
  fixture.deliver_frame(kHelperActions, 1u, act_declare(1u, "spin"));
  fixture.deliver_frame(kHelperActions, 2u, act_declare(2u, "old"));
  fixture.deliver_frame(kHelperActions, 3u, act_declare(3u, "missing"));
  fixture.deliver_frame(kHelperActions, 4u, {kHelperActStatus, 0x00u, 0x04u, 0x01u});
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 9u);
  EXPECT_EQ(units[0], with_check({kHelperActions, 1u, kHelperActStatus, 0x00u, 0x00u, 0x00u}));
  EXPECT_EQ(units[2], with_check({kHelperActions, 3u, kHelperActStatus, 0x00u, 0x01u, 0x03u}));
  EXPECT_EQ(units[4], with_check({kHelperActions, 5u, kHelperActStatus, 0x00u, 0x02u, 0x02u}));
  EXPECT_EQ(units[6], with_check({kHelperActions, 7u, kHelperActStatus, 0x00u, 0x03u, 0x01u}));
  ASSERT_EQ(fixture.session().actions().size(), 4u);
  EXPECT_EQ(fixture.session().actions()[0].status, kActionStatusOk);
  EXPECT_EQ(fixture.session().actions()[1].status, kActionStatusNameConflict);
  EXPECT_EQ(fixture.session().actions()[2].status, kActionStatusHashMismatch);
  EXPECT_EQ(fixture.session().actions()[3].status, kActionStatusUnknownType);
}

// covers: ACT-7
TEST(ActionWalk, GivesUpAfterThreeRetries)
{
  SessionConfig config;
  config.heartbeat_period_ms = 3600000;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  for (int retry = 0; retry < 3; ++retry) {
    fixture.advance_to(200u * static_cast<std::uint32_t>(retry + 1u) + 100u);
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u);
    EXPECT_EQ(units[0][2], kHelperActQuery) << "the walk retries the QUERY";
  }
  fixture.advance_to(900u);
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(units[0][2], 0x01u) << "a HELLO_REQ, not another ACT_QUERY";
}

// covers: ACT-7
TEST(ActionWalk, ReportsUnknownTypeWithoutAResolver)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{[](const TopicInfo &) {return std::uint8_t{0};}, config};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 0u, act_declare(0u, "spin"));
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 2u);
  EXPECT_EQ(units[0], with_check({kHelperActions, 1u, kHelperActStatus, 0x00u, 0x00u, 0x01u}));
}

// covers: ACT-3, ACT-4
TEST(ActionExchange, SendsGoalsOnTheirOwnStream)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 2u, act_declare(0u, "spin"));
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  drain_decoded(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 0x09u, {0x01u}));
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 2u);
  EXPECT_EQ(units[0], with_check({kHelperServices, 0u, kHelperSvcRequest, 0x00u, 0x09u, 0x00u,
      0x01u}));
  EXPECT_EQ(units[1], with_check({kHelperActions, 3u, kHelperActGoal, 0x00u, 0x00u, 0x00u,
      0x05u})) << "the action frame seq continues the walk, its rel_seq starts at 0";
}

// covers: ACT-3
TEST(ActionExchange, QueuesOnePendingGoalPerIndex)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x01u}));
  drain_decoded(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x01u, {0x02u}));
  EXPECT_EQ(fixture.session().reliable_replaced(), 0u);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x02u, {0x03u}));
  EXPECT_EQ(fixture.session().reliable_replaced(), 1u) << "a newer GOAL replaces the pending one";
  fixture.deliver_frame(kAck, 0u, {kHelperActions, 0x00u});
  fixture.session().poll();
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(units[0], with_check({kHelperActions, 4u, kHelperActGoal, 0x01u, 0x02u, 0x00u,
      0x03u}));
}

// covers: ACT-3, ACT-4
TEST(ActionExchange, AppliesGoalResponsesInEitherOrder)
{
  std::vector<std::uint8_t> answered;
  SessionEvents events;
  events.action_goal_response = [&](std::uint8_t seq, std::uint8_t) {
      answered.push_back(seq);
    };
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x01u}));
  drain_decoded(fixture);
  // The second GOAL queues behind the unacked first and goes out once the
  // MCU acks rel 0; both stay tracked until their own GOAL_RESP arrives.
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x01u, {0x02u}));
  fixture.deliver_frame(kAck, 0u, {kHelperActions, 0x00u});
  fixture.session().poll();
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActGoalResp, 0x00u, 0x01u, 0x00u});
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x01u, 0x00u, 0x00u});
  ASSERT_EQ(answered.size(), 2u);
  EXPECT_EQ(answered[0], 0x01u) << "the later goal's answer applies first";
  EXPECT_EQ(answered[1], 0x00u);
}

// covers: ACT-4
TEST(ActionExchange, AppliesTheGoalResponseOnce)
{
  int calls = 0;
  std::uint8_t decision = 0xFFu;
  SessionEvents events;
  events.action_goal_response = [&](std::uint8_t, std::uint8_t got) {
      ++calls;
      decision = got;
    };
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  EXPECT_EQ(calls, 1) << "the duplicate applies once";
  EXPECT_EQ(decision, 0x00u);
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 2u);
  EXPECT_EQ(units[0], with_check({kAck, 0u, kHelperActions, 0x00u}));
  EXPECT_EQ(units[1], with_check({kAck, 1u, kHelperActions, 0x00u}))
    << "every reliable frame is acked, even the duplicate";
}

// covers: ACT-4
TEST(ActionExchange, RefusesABadGoalResponseDecision)
{
  int calls = 0;
  SessionEvents events;
  events.action_goal_response = [&](std::uint8_t, std::uint8_t) {++calls;};
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActGoalResp, 0x00u, 0x00u, 0x04u});
  EXPECT_EQ(calls, 0) << "decision 4 is reserved and refused";
}

// covers: ACT-5
TEST(ActionExchange, ReportsTheCancelResponse)
{
  int calls = 0;
  std::uint8_t decision = 0xFFu;
  SessionEvents events;
  events.action_cancel_response = [&](std::uint8_t, std::uint8_t got) {
      ++calls;
      decision = got;
    };
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kAck, 0u, {kHelperActions, 0x00u});
  fixture.session().poll();
  drain_decoded(fixture);
  EXPECT_TRUE(fixture.session().send_action_cancel(0x00u));
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActCancelResp, 0x01u, 0x00u, 0x00u});
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(decision, 0x00u);
}

// covers: ACT-1
TEST(ActionExchange, DropsFeedbackForAnUnservedIndex)
{
  int calls = 0;
  SessionEvents events;
  events.action_feedback = [&](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &) {
      ++calls;
    };
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActFeedback, 0x00u, 0x07u, 0x01u});
  EXPECT_EQ(calls, 0) << "feedback for an unknown index is dropped";
  EXPECT_TRUE(drain_decoded(fixture).empty()) << "feedback is never acked";
}

// covers: ACT-1
TEST(ActionExchange, ReportsFeedbackAndResult)
{
  std::vector<std::uint8_t> feedback;
  std::uint8_t outcome = 0xFFu;
  std::vector<std::uint8_t> result;
  SessionEvents events;
  events.action_feedback = [&](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> & got) {
      feedback = got;
    };
  events.action_result =
    [&](std::uint8_t, std::uint8_t got, const std::vector<std::uint8_t> & got_payload) {
      outcome = got;
      result = got_payload;
    };
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActFeedback, 0x00u, 0x00u, 0x09u});
  EXPECT_EQ(feedback, std::vector<std::uint8_t>({0x09u}));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActResult, 0x00u, 0x00u, 0x04u, 0x07u});
  EXPECT_EQ(outcome, 0x04u);
  EXPECT_EQ(result, std::vector<std::uint8_t>({0x07u}));
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u) << "only the RESULT is acked";
  EXPECT_EQ(units[0], with_check({kAck, 0u, kHelperActions, 0x00u}));
}

// covers: ACT-6
TEST(ActionExchange, RefusesAReservedOutcome)
{
  int calls = 0;
  SessionEvents events;
  events.action_result = [&](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &) {
      ++calls;
    };
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActResult, 0x00u, 0x00u, 0x03u, 0x07u});
  EXPECT_EQ(calls, 0) << "outcome 3 is reserved and refused";
}

// covers: ACT-3
TEST(ActionExchange, RefusesToSendWhileNotConnected)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  EXPECT_FALSE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  EXPECT_FALSE(fixture.session().send_action_cancel(0x00u));
}

// covers: ACT-3
TEST(ActionExchange, RefusesToSendForAnUnknownAction)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_FALSE(fixture.session().send_action_goal(0x03u, 0x00u, {0x05u}));
}

// covers: REL-2
TEST(ActionExchange, RetriesThenGivesUp)
{
  SessionConfig config = quiet_config();
  config.link_timeout_ms = 3600000;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  drain_decoded(fixture);
  for (int retry = 0; retry < 3; ++retry) {
    fixture.advance_to(500u * static_cast<std::uint32_t>(retry + 1u));
    EXPECT_EQ(drain_decoded(fixture).size(), 1u) << "the GOAL retries with its rel_seq";
  }
  fixture.advance_to(2500u);
  EXPECT_TRUE(drain_decoded(fixture).empty()) << "spent retries drop the GOAL";
  EXPECT_EQ(fixture.session().reliable_dropped(), 1u);
}

// covers: REL-5
TEST(ActionExchange, DropsActionStateOnRehandshake)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  drain_decoded(fixture);
  fixture.deliver_frame(kControl, 5u, hello_payload(0x44u, 1u, 4u, 0xCCu, 1000u));
  EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
  EXPECT_FALSE(fixture.session().send_action_goal(0u, 0x01u, {0x05u}))
    << "a re-handshake drops the reliable state";
}

// covers: ACT-1
TEST(ActionExchange, IgnoresAnUnknownActionOpcode)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  fixture.deliver_frame(kHelperActions, 1u, {0x0Au, 0x00u, 0x00u});
  EXPECT_TRUE(drain_decoded(fixture).empty());
}

// covers: ACT-7
TEST(ActionWalk, DropsMalformedDeclares)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  // A DECLARE cut after three hash bytes parses nothing: the walk answers
  // unknown type and moves on.
  fixture.deliver_frame(kHelperActions, 0u, {kHelperActDeclare, 0x00u, 0x01u, 0x02u, 0x03u});
  // A DECLARE with a full hash but a truncated name parses nothing either.
  std::vector<std::uint8_t> short_name{kHelperActDeclare, 0x01u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u,
    4u, 's', 'p'};
  fixture.deliver_frame(kHelperActions, 1u, short_name);
  fixture.deliver_frame(kHelperActions, 2u, act_declare(2u, "spin"));
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActStatus, 0x00u, 0x03u, 0x01u});
  const auto units = drain_decoded(fixture);
  // Each DECLARE is answered and the walk queries on: three STATUS answers
  // interleaved with three QUERY frames, then the READY that ends it.
  ASSERT_EQ(units.size(), 7u);
  EXPECT_EQ(units[0], with_check({kHelperActions, 1u, kHelperActStatus, 0x00u, 0x00u, 0x01u}));
  EXPECT_EQ(units[2], with_check({kHelperActions, 3u, kHelperActStatus, 0x00u, 0x01u, 0x01u}));
  EXPECT_EQ(units[4], with_check({kHelperActions, 5u, kHelperActStatus, 0x00u, 0x02u, 0x00u}));
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: ACT-7
TEST(ActionWalk, IgnoresABadStatusAndAdvancesOnZero)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  // A DECLARE before the walk starts and a STATUS while the walk has not
  // reached actions are both ignored.
  fixture.deliver_frame(kHelperActions, 1u, act_declare(0u, "spin"));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActStatus, 0x00u, 0x00u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  // A short STATUS, one for the wrong index and one with a reserved
  // decision are ignored, as is a DECLARE for an index the walk is not
  // asking for.
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActStatus, 0x00u});
  fixture.deliver_frame(kHelperActions, 4u, {kHelperActStatus, 0x00u, 0x05u, 0x01u});
  fixture.deliver_frame(kHelperActions, 5u, {kHelperActStatus, 0x00u, 0x00u, 0x07u});
  fixture.deliver_frame(kHelperActions, 6u, act_declare(9u, "spin"));
  // A STATUS 0 advances the walk to the next index.
  fixture.deliver_frame(kHelperActions, 7u, {kHelperActStatus, 0x00u, 0x00u, 0x00u});
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(units[0], with_check({kHelperActions, 1u, kHelperActQuery, 0x01u}))
    << "the walk asks for index 1 next";
  fixture.deliver_frame(kHelperActions, 8u, act_declare(1u, "spin"));
  fixture.deliver_frame(kHelperActions, 9u, {kHelperActStatus, 0x00u, 0x02u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  // A STATUS after the walk ended is ignored.
  fixture.deliver_frame(kHelperActions, 10u, {kHelperActStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: ACT-1, ACT-4, ACT-5, ACT-6
TEST(ActionWalk, IgnoresActionFramesWhileWalking)
{
  int calls = 0;
  SessionEvents events;
  events.action_goal_response = [&](std::uint8_t, std::uint8_t) {++calls;};
  events.action_cancel_response = [&](std::uint8_t, std::uint8_t) {++calls;};
  events.action_feedback =
    [&](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &) {++calls;};
  events.action_result =
    [&](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &) {++calls;};
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  // Action frames before the walk reaches actions are all ignored, as is
  // a short RESULT.
  fixture.deliver_frame(kHelperActions, 0u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActCancelResp, 0x00u, 0x00u, 0x00u});
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActFeedback, 0x00u, 0x00u, 0x09u});
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActResult, 0x00u, 0x00u, 0x04u, 0x09u});
  fixture.deliver_frame(kHelperActions, 4u, {kHelperActResult, 0x00u, 0x00u});
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
}

// covers: ACT-7
TEST(ActionWalk, RefusesADeclareWithATruncatedName)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  // A valid type name with a cut-off action name parses nothing: the walk
  // answers unknown type and moves on.
  std::vector<std::uint8_t> cut = act_declare(0u, "spin");
  cut.pop_back();
  cut.pop_back();
  fixture.deliver_frame(kHelperActions, 0u, cut);
  fixture.deliver_frame(kHelperActions, 1u, act_declare(1u, "spin"));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActStatus, 0x00u, 0x02u, 0x01u});
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 5u);
  EXPECT_EQ(units[0], with_check({kHelperActions, 1u, kHelperActStatus, 0x00u, 0x00u, 0x01u}));
  EXPECT_EQ(units[2], with_check({kHelperActions, 3u, kHelperActStatus, 0x00u, 0x01u, 0x00u}));
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: ACT-7
TEST(ActionWalk, RefusesADeclareWithTrailingBytes)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  // Valid strings with a byte left over parse nothing: the walk answers
  // unknown type and moves on.
  std::vector<std::uint8_t> trailing = act_declare(0u, "spin");
  trailing.push_back(0x00u);
  fixture.deliver_frame(kHelperActions, 0u, trailing);
  fixture.deliver_frame(kHelperActions, 1u, act_declare(1u, "spin"));
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActStatus, 0x00u, 0x02u, 0x01u});
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 5u);
  EXPECT_EQ(units[0], with_check({kHelperActions, 1u, kHelperActStatus, 0x00u, 0x00u, 0x01u}));
  EXPECT_EQ(units[2], with_check({kHelperActions, 3u, kHelperActStatus, 0x00u, 0x01u, 0x00u}));
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: ACT-3
TEST(ActionExchange, PromotesAPendingCancelOnAck)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  drain_decoded(fixture);
  // An ack for another rel_seq applies to nothing.
  fixture.deliver_frame(kAck, 0u, {kHelperActions, 0x09u});
  fixture.session().poll();
  EXPECT_TRUE(drain_decoded(fixture).empty());
  // The CANCEL queues behind the unacked GOAL; a second CANCEL replaces it.
  EXPECT_TRUE(fixture.session().send_action_cancel(0x00u));
  EXPECT_TRUE(fixture.session().send_action_cancel(0x01u));
  EXPECT_EQ(fixture.session().reliable_replaced(), 1u);
  EXPECT_TRUE(drain_decoded(fixture).empty()) << "nothing goes out before the ack";
  fixture.deliver_frame(kAck, 0u, {kHelperActions, 0x00u});
  fixture.session().poll();
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(units[0], with_check({kHelperActions, 4u, kHelperActCancel, 0x01u, 0x01u}))
    << "the ack promotes the newest pending CANCEL";
}

// covers: REL-2
TEST(ActionExchange, DropsAnUnackedCancelAfterRetries)
{
  int calls = 0;
  SessionEvents events;
  events.action_cancel_response = [&](std::uint8_t, std::uint8_t) {++calls;};
  SessionConfig config = quiet_config();
  config.link_timeout_ms = 3600000;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  drain_decoded(fixture);
  fixture.deliver_frame(kAck, 0u, {kHelperActions, 0x00u});
  fixture.session().poll();
  drain_decoded(fixture);
  EXPECT_TRUE(fixture.session().send_action_cancel(0x00u));
  drain_decoded(fixture);
  for (int retry = 0; retry < 3; ++retry) {
    fixture.advance_to(500u * static_cast<std::uint32_t>(retry + 1u));
    EXPECT_EQ(drain_decoded(fixture).size(), 1u) << "the CANCEL retries with its rel_seq";
  }
  fixture.advance_to(2500u);
  EXPECT_TRUE(drain_decoded(fixture).empty()) << "spent retries drop the CANCEL";
  EXPECT_EQ(fixture.session().reliable_dropped(), 1u);
  // A late CANCEL_RESP for the dropped CANCEL names an untracked goal: it
  // carries rel 0, the first rel the link sees.
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActCancelResp, 0x00u, 0x00u, 0x00u});
  EXPECT_EQ(calls, 0);
}

// covers: ACT-5
TEST(ActionExchange, RefusesAReservedCancelDecision)
{
  int calls = 0;
  SessionEvents events;
  events.action_cancel_response = [&](std::uint8_t, std::uint8_t) {++calls;};
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  drain_decoded(fixture);
  EXPECT_TRUE(fixture.session().send_action_cancel(0x00u));
  drain_decoded(fixture);
  // Decision 3 is reserved and refused, like an unknown goal.
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActCancelResp, 0x00u, 0x00u, 0x03u});
  EXPECT_EQ(calls, 0);
}

// covers: ACT-1, ACT-4, ACT-5, ACT-6
TEST(ActionExchange, IgnoresStaleActionFrames)
{
  int goal_calls = 0;
  int cancel_calls = 0;
  int feedback_calls = 0;
  int result_calls = 0;
  SessionEvents events;
  events.action_goal_response = [&](std::uint8_t, std::uint8_t) {++goal_calls;};
  events.action_cancel_response = [&](std::uint8_t, std::uint8_t) {++cancel_calls;};
  events.action_feedback = [&](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &) {
      ++feedback_calls;
    };
  events.action_result = [&](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &) {
      ++result_calls;
    };
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  // An empty action frame, a DECLARE outside the walk, a short CANCEL_RESP
  // and a short FEEDBACK are all refused.
  fixture.deliver_frame(kHelperActions, 1u, {});
  fixture.deliver_frame(kHelperActions, 2u, act_declare(0u, "spin"));
  fixture.deliver_frame(kHelperActions, 3u, {kHelperActCancelResp, 0x00u});
  fixture.deliver_frame(kHelperActions, 4u, {kHelperActFeedback, 0x00u});
  EXPECT_EQ(goal_calls + cancel_calls + feedback_calls + result_calls, 0);
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  // Track a GOAL and a CANCEL, then answer each twice and with an unknown
  // seq: only the first answers apply.
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 5u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  EXPECT_EQ(goal_calls, 1);
  fixture.deliver_frame(kAck, 0u, {kHelperActions, 0x00u});
  fixture.session().poll();
  drain_decoded(fixture);
  EXPECT_TRUE(fixture.session().send_action_cancel(0x00u));
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 6u, {kHelperActCancelResp, 0x01u, 0x00u, 0x00u});
  EXPECT_EQ(cancel_calls, 1);
  fixture.deliver_frame(kHelperActions, 7u, {kHelperActCancelResp, 0x01u, 0x00u, 0x00u});
  fixture.deliver_frame(kHelperActions, 8u, {kHelperActCancelResp, 0x02u, 0x09u, 0x00u});
  EXPECT_EQ(cancel_calls, 1) << "a duplicate and an unknown seq apply nothing";
  // A GOAL_RESP for an untracked seq applies nothing either.
  fixture.deliver_frame(kHelperActions, 9u, {kHelperActGoalResp, 0x03u, 0x07u, 0x00u});
  EXPECT_EQ(goal_calls, 1) << "the earlier accept stands; the stray applies nothing";
  // A RESULT with a stale rel and a stray ack are ignored too.
  fixture.deliver_frame(kHelperActions, 10u, {kHelperActResult, 0x00u, 0x00u, 0x04u, 0x07u});
  fixture.deliver_frame(kAck, 1u, {kHelperActions, 0x09u});
  EXPECT_EQ(result_calls, 0);
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: ACT-7
TEST(ActionExchange, RefusesAGoalForARefusedAction)
{
  int calls = 0;
  SessionEvents events;
  events.action_feedback = [&](std::uint8_t, std::uint8_t, const std::vector<std::uint8_t> &) {
      ++calls;
    };
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusUnknownType;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0xCCu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperActions, 0u, act_declare(0u, "spin"));
  fixture.deliver_frame(kHelperActions, 1u, {kHelperActStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  EXPECT_FALSE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}))
    << "a refused action takes no goals";
  // Feedback for a refused action is dropped too.
  fixture.deliver_frame(kHelperActions, 2u, {kHelperActFeedback, 0x00u, 0x00u, 0x09u});
  EXPECT_EQ(calls, 0);
}

// covers: REL-2, REL-5
TEST(ActionExchange, DropsAQueuedGoalOnRehandshake)
{
  int calls = 0;
  SessionEvents events;
  events.action_goal_response = [&](std::uint8_t, std::uint8_t) {++calls;};
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  // The link wedges with the GOAL still queued: the re-handshake drops the
  // reliable state, answering nothing, and the late GOAL_RESP is ignored.
  fixture.block_writes();
  EXPECT_TRUE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}));
  fixture.deliver_frame(kControl, 5u, hello_payload(0x44u, 1u, 4u, 0xCCu, 1000u));
  EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
  fixture.deliver_frame(kHelperActions, 5u, {kHelperActGoalResp, 0x00u, 0x00u, 0x00u});
  EXPECT_EQ(calls, 0);
  // Unwedging restarts the walk from scratch: the dropped GOAL is gone.
  fixture.allow_writes();
  fixture.session().poll();
  for (const auto & unit : drain_decoded(fixture)) {
    EXPECT_TRUE(unit.size() < 3u || unit[2] != kHelperActGoal)
      << "the queued GOAL never goes out";
  }
}

// covers: ACT-3
TEST(ActionExchange, RefusesAnOversizeGoal)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  EXPECT_FALSE(fixture.session().send_action_goal(0u, 0x00u,
          std::vector<std::uint8_t>(600u, 0xAAu)))
    << "a goal past the MCU window never leaves";
}

// covers: ACT-3
TEST(ActionExchange, RefusesCallsOnAFullLink)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;},
    [](const ActionInfo &) {return kActionStatusOk;}};
  handshake_with_actions(fixture);
  fixture.block_writes();
  // Topics 16 and up never coalesce, so both frames stay queued.
  EXPECT_TRUE(fixture.session().send_user(16u, std::vector<std::uint8_t>(400u, 0xABu)));
  EXPECT_TRUE(fixture.session().send_user(17u, std::vector<std::uint8_t>(200u, 0xABu)));
  EXPECT_FALSE(fixture.session().send_action_goal(0u, 0x00u, {0x05u}))
    << "a queued GOAL would overflow the link window";
  EXPECT_FALSE(fixture.session().send_action_cancel(0x00u))
    << "a queued CANCEL would overflow the link window";
}

// covers: SVR-7
TEST(ServiceWalk, StartsAfterTheParameterWalk)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  const auto walked = drain_decoded(fixture);
  ASSERT_EQ(walked.size(), 3u);
  EXPECT_EQ(walked[2][0], kParams) << "the parameter walk still goes first";
  fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(units[0][0], kHelperServices);
  EXPECT_EQ(units[0][2], kHelperSvcQuery) << "the service walk starts with index 0";
  EXPECT_EQ(units[0][3], 0x00u);
  EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
}

// covers: SVR-7
TEST(ServiceWalk, SkipsWhenTheMcuAnnouncesNoServices)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x04u, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 4u);
  EXPECT_EQ(units[3][2], 0x06u) << "READY goes out with no service walk";
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: SVR-7
TEST(ServiceWalk, RecordsVerdicts)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo & info) {
      if (info.service_name == "reset") {
        return kServiceStatusOk;
      }
      if (info.service_name == "old") {
        return kServiceStatusHashMismatch;
      }
      return kServiceStatusUnknownType;
    }};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 1u, svc_declare(1u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 2u, svc_declare(2u, 0x00u, "old"));
  fixture.deliver_frame(kHelperServices, 3u, svc_declare(3u, 0x00u, "missing"));
  fixture.deliver_frame(kHelperServices, 4u, {kHelperSvcStatus, 0x00u, 0x04u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  const std::vector<ServiceInfo> & services = fixture.session().services();
  ASSERT_EQ(services.size(), 4u);
  EXPECT_EQ(services[0].status, kServiceStatusOk);
  EXPECT_EQ(services[1].status, kServiceStatusNameConflict) << "a repeated name conflicts";
  EXPECT_EQ(services[2].status, kServiceStatusHashMismatch);
  EXPECT_EQ(services[3].status, kServiceStatusUnknownType);
  // The topic walk, its STATUS, the parameter REQ, then the service walk:
  // a QUERY and a plain STATUS per index, and READY.
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 13u);
  EXPECT_EQ(units[4], with_check({kHelperServices, 1u, kHelperSvcStatus, 0x00u, 0x00u, 0x00u}));
  EXPECT_EQ(units[6], with_check({kHelperServices, 3u, kHelperSvcStatus, 0x00u, 0x01u, 0x03u}));
  EXPECT_EQ(units[8], with_check({kHelperServices, 5u, kHelperSvcStatus, 0x00u, 0x02u, 0x02u}));
  EXPECT_EQ(units[10], with_check({kHelperServices, 7u, kHelperSvcStatus, 0x00u, 0x03u, 0x01u}));
}

// covers: SVR-1, SVR-2, SVR-7
TEST(ServiceWalk, SkipsUnparseableDeclares)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  // A short payload, an unknown role and truncated names keep no entry,
  // while the walk still answers and advances.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcDeclare, 0x00u});
  const std::array<std::uint8_t, 8> hash{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
  const std::string type = "std_srvs/srv/Trigger";
  std::vector<std::uint8_t> bad_role{kHelperSvcDeclare, 0x01u, 0x05u};
  bad_role.insert(bad_role.end(), hash.begin(), hash.end());
  bad_role.push_back(static_cast<std::uint8_t>(type.size()));
  bad_role.insert(bad_role.end(), type.begin(), type.end());
  bad_role.push_back(0x05u);
  bad_role.insert(bad_role.end(), {'r', 'e', 's', 'e', 't'});
  fixture.deliver_frame(kHelperServices, 1u, bad_role);
  std::vector<std::uint8_t> truncated{kHelperSvcDeclare, 0x02u, 0x00u};
  truncated.insert(truncated.end(), hash.begin(), hash.end());
  truncated.push_back(static_cast<std::uint8_t>(type.size()));
  truncated.insert(truncated.end(), type.begin(), type.end() - 1u);
  fixture.deliver_frame(kHelperServices, 2u, truncated);
  fixture.deliver_frame(kHelperServices, 3u, svc_declare(0x03u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 4u, {kHelperSvcStatus, 0x00u, 0x04u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  const std::vector<ServiceInfo> & services = fixture.session().services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].index, 3u);
  EXPECT_EQ(services[0].status, kServiceStatusOk);
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 9u);
  EXPECT_EQ(units[0], with_check({kHelperServices, 1u, kHelperSvcStatus, 0x00u, 0x00u, 0x01u}));
  EXPECT_EQ(units[2], with_check({kHelperServices, 3u, kHelperSvcStatus, 0x00u, 0x01u, 0x01u}));
  EXPECT_EQ(units[4], with_check({kHelperServices, 5u, kHelperSvcStatus, 0x00u, 0x02u, 0x01u}));
  EXPECT_EQ(units[6], with_check({kHelperServices, 7u, kHelperSvcStatus, 0x00u, 0x03u, 0x00u}));
}

// covers: SVR-7
TEST(ServiceWalk, GivesUpAfterThreeRetries)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  EXPECT_EQ(drain_decoded(fixture).size(), 4u);
  for (int retry = 0; retry < 3; ++retry) {
    fixture.advance_to(200u * static_cast<std::uint32_t>(retry + 1u));
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "retry " << retry;
    EXPECT_EQ(units[0][2], kHelperSvcQuery) << "the walk retries the QUERY";
  }
  fixture.advance_to(800);
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u) << "after the third retry the handshake starts over";
  EXPECT_EQ(units[0][2], 0x01u) << "a HELLO_REQ, not another SVC_QUERY";
}

// covers: SVR-7
TEST(ServiceWalk, IgnoresStrayDeclaresBeforeTheWalk)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  // A DECLARE before the service walk starts never joins the table.
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  // A short DECLARE and one for another index never join it either.
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcDeclare});
  fixture.deliver_frame(kHelperServices, 2u, svc_declare(5u, 0x00u, "reset"));
  // The walk still completes on the valid frames.
  fixture.deliver_frame(kHelperServices, 3u, svc_declare(0u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 4u, svc_declare(1u, 0x01u, "trigger"));
  fixture.deliver_frame(kHelperServices, 5u, {kHelperSvcStatus, 0x00u, 0x02u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  const std::vector<ServiceInfo> & services = fixture.session().services();
  ASSERT_EQ(services.size(), 2u);
  EXPECT_EQ(services[0].index, 0u);
  EXPECT_EQ(services[1].index, 1u);
}

// covers: SVR-7
TEST(ServiceWalk, IgnoresStrayFramesAroundTheWalk)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  // A STATUS before the service walk starts is not awaited: ignored.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x00u});
  // A RESPONSE before any call exists arrives handshaking: ignored.
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcResponse, 0x00u, 0x09u, 0x00u});
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 2u, svc_declare(0u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 3u, {kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  ASSERT_EQ(fixture.session().services().size(), 1u);
  drain_decoded(fixture);
  // A STATUS while connected answers nothing and changes nothing.
  fixture.deliver_frame(kHelperServices, 4u, {kHelperSvcStatus, 0x00u, 0x00u, 0x00u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  EXPECT_EQ(fixture.session().services().size(), 1u);
  EXPECT_TRUE(drain_decoded(fixture).empty()) << "no answer for a stray STATUS";
}

// covers: SVR-7
TEST(ServiceWalk, SkipsADeclareWithABadNameAndTrailingBytes)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  // The walk still completes around frames that never parse.
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  // A valid type with a truncated service name never parses.
  const std::array<std::uint8_t, 8> hash{1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
  const std::string type = "std_srvs/srv/Trigger";
  std::vector<std::uint8_t> bad_name{kHelperSvcDeclare, 0x01u, 0x00u};
  bad_name.insert(bad_name.end(), hash.begin(), hash.end());
  bad_name.push_back(static_cast<std::uint8_t>(type.size()));
  bad_name.insert(bad_name.end(), type.begin(), type.end());
  bad_name.push_back(0x05u);
  bad_name.insert(bad_name.end(), {'r', 'e', 's'});
  fixture.deliver_frame(kHelperServices, 1u, bad_name);
  // A valid DECLARE with trailing bytes never parses either.
  std::vector<std::uint8_t> trailing = svc_declare(2u, 0x00u, "trigger");
  trailing.push_back(0x00u);
  fixture.deliver_frame(kHelperServices, 2u, trailing);
  fixture.deliver_frame(kHelperServices, 3u, {kHelperSvcStatus, 0x00u, 0x03u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  const std::vector<ServiceInfo> & services = fixture.session().services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].index, 0u);
}

// covers: SVR-7
TEST(ServiceWalk, ReportsUnknownTypeWithoutAResolver)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  // No service resolver: every verdict is unknown type.
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  const auto units = drain_decoded(fixture);
  const auto status = std::find_if(units.begin(), units.end(), [](const auto & unit) {
        return unit.size() >= 6u && unit[0] == kHelperServices && unit[2] == kHelperSvcStatus;
  });
  ASSERT_TRUE(status != units.end()) << "the walk answers the DECLARE";
  EXPECT_EQ(
    *status, with_check({kHelperServices, 1u, kHelperSvcStatus, 0x00u, 0x00u, 0x01u}))
    << "no resolver means unknown type";
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  const std::vector<ServiceInfo> & services = fixture.session().services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].status, kServiceStatusUnknownType);
}

// covers: SVR-7
TEST(ServiceWalk, ReportsAResolverConflict)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  // A resolver may report a conflict itself: the STATUS carries it.
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusNameConflict;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  const auto units = drain_decoded(fixture);
  const auto status = std::find_if(units.begin(), units.end(), [](const auto & unit) {
        return unit.size() >= 6u && unit[0] == kHelperServices && unit[2] == kHelperSvcStatus;
  });
  ASSERT_TRUE(status != units.end()) << "the walk answers the DECLARE";
  EXPECT_EQ(
    *status, with_check({kHelperServices, 1u, kHelperSvcStatus, 0x00u, 0x00u, 0x03u}))
    << "a resolver conflict answers conflict";
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: REL-2, REL-3, SVR-4, SVR-10
TEST(ServiceExchange, SendsRequestsOnTheirOwnStream)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u,
    {kParamDeclare, 0x00u, 0x00u, 0x00u, 0x03u, 'l', 'e', 'd', 0x01u});
  fixture.deliver_frame(kParams, 1u, {kParamStatus, 0x00u, 0x01u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcStatus, 0x00u, 0x01u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  drain_decoded(fixture);
  // One parameter SET stays outstanding: the service REQUEST still goes
  // out at once, on its own rel seq stream, with connected frame seqs.
  EXPECT_TRUE(fixture.session().send_param_set(0u, {0x01u}));
  EXPECT_TRUE(fixture.session().send_service_request(0u, 9u, {}));
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 2u);
  EXPECT_EQ(units[0], with_check({kParams, 0u, kParamSet, 0x00u, 0x00u, 0x01u}));
  EXPECT_EQ(units[1], with_check({kHelperServices, 0u, kHelperSvcRequest, 0x00u, 0x09u, 0x00u}));
}

// covers: SVR-10
TEST(ServiceExchange, QueuesOnePendingRequestPerIndex)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 1u, {0xAAu}));
  EXPECT_TRUE(fixture.session().send_service_request(1u, 2u, {0xBBu}));
  EXPECT_TRUE(fixture.session().send_service_request(1u, 3u, {0xCCu}));
  EXPECT_EQ(fixture.session().reliable_replaced(), 1u) << "the newer REQUEST replaces";
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u) << "only the outstanding REQUEST goes out";
  // The ack frees the stream, and the pending REQUEST for the other index
  // goes out with the next rel seq.
  fixture.deliver_frame(kAck, 0u, {kHelperServices, 0x00u});
  const auto next = drain_decoded(fixture);
  ASSERT_EQ(next.size(), 1u);
  EXPECT_EQ(
    next[0],
    with_check({kHelperServices, 1u, kHelperSvcRequest, 0x01u, 0x03u, 0x01u, 0xCCu}));
}

// covers: SVR-5
TEST(ServiceExchange, AnswersOnlyTheOutstandingCall)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> requests;
  SessionEvents events;
  events.service_request =
    [&requests](std::uint8_t index, std::uint8_t req, const std::vector<std::uint8_t> & payload) {
      requests.emplace_back(index, req, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcRequest, 0x00u, 0x07u, 0x00u});
  ASSERT_EQ(requests.size(), 1u);
  // A second MCU REQUEST while the first is outstanding is acked and
  // dropped: the first stays answerable, and the second never surfaces.
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcRequest, 0x01u, 0x08u, 0x00u});
  EXPECT_EQ(requests.size(), 1u);
  const auto drops = drain_decoded(fixture);
  ASSERT_EQ(drops.size(), 2u);
  EXPECT_EQ(drops[0], with_check({kAck, 0u, kHelperServices, 0x00u}));
  EXPECT_EQ(drops[1], with_check({kAck, 1u, kHelperServices, 0x01u}));
  // The wrong req_id sends nothing.
  EXPECT_FALSE(fixture.session().send_service_response(8u, 0u, {}));
  // The outstanding one answers on the reliable channel, exactly once.
  EXPECT_TRUE(fixture.session().send_service_response(7u, 0u, {0x01u}));
  EXPECT_FALSE(fixture.session().send_service_response(7u, 0u, {0x02u}));
  EXPECT_FALSE(fixture.session().send_service_response(8u, 0u, {}));
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(
    units[0],
    with_check({kHelperServices, 0u, kHelperSvcResponse, 0x00u, 0x07u, 0x00u, 0x01u}));
  // A REQUEST for an index the walk never announced is acked and dropped:
  // nothing is served and no event fires.
  fixture.deliver_frame(kHelperServices, 2u, {kHelperSvcRequest, 0x02u, 0x09u, 0x09u});
  EXPECT_EQ(requests.size(), 1u);
  const auto unknowns = drain_decoded(fixture);
  ASSERT_EQ(unknowns.size(), 1u);
  EXPECT_EQ(unknowns[0], with_check({kAck, 2u, kHelperServices, 0x02u}));
}

// covers: SVR-5
TEST(ServiceExchange, DropsStaleMcuResponses)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> responses;
  SessionEvents events;
  events.service_response =
    [&responses](std::uint8_t req, std::uint8_t status, const std::vector<std::uint8_t> & payload) {
      responses.emplace_back(req, status, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 4u, {}));
  drain_decoded(fixture);
  // A RESPONSE for a retired req_id is acked and dropped.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcResponse, 0x00u, 0x09u, 0x00u, 0x01u});
  EXPECT_TRUE(responses.empty());
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(units[0], with_check({kAck, 0u, kHelperServices, 0x00u}));
  // The outstanding call still answers, on the next rel seq.
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcResponse, 0x01u, 0x04u, 0x00u, 0x02u});
  ASSERT_EQ(responses.size(), 1u);
}

// covers: SVR-9
TEST(ServiceExchange, RefusesToSendWhileNotConnected)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  EXPECT_FALSE(fixture.session().send_service_request(0u, 4u, {}))
      << "no REQUEST leaves a boot-waiting session";
  EXPECT_FALSE(fixture.session().send_service_response(4u, 0u, {}))
      << "no RESPONSE leaves a boot-waiting session";
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0x4Cu, 1000u));
  EXPECT_FALSE(fixture.session().send_service_request(0u, 4u, {}))
      << "no REQUEST leaves a handshaking session";
  EXPECT_FALSE(fixture.session().send_service_response(4u, 0u, {}))
      << "no RESPONSE leaves a handshaking session";
}

// covers: SVR-6
TEST(ServiceExchange, AppliesNonzeroStatusesEmpty)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> responses;
  SessionEvents events;
  events.service_response =
    [&responses](std::uint8_t req, std::uint8_t status, const std::vector<std::uint8_t> & payload) {
      responses.emplace_back(req, status, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 4u, {}));
  drain_decoded(fixture);
  // A nonzero status answers empty: the event fires with no payload.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcResponse, 0x00u, 0x04u, 0x02u});
  ASSERT_EQ(responses.size(), 1u);
  EXPECT_EQ(std::get<0>(responses[0]), 0x04u);
  EXPECT_EQ(std::get<1>(responses[0]), 0x02u);
  EXPECT_TRUE(std::get<2>(responses[0]).empty());
  drain_decoded(fixture);
  // A reserved status is refused: no event and not even an ack.
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcResponse, 0x01u, 0x04u, 0x04u});
  EXPECT_EQ(responses.size(), 1u);
  EXPECT_TRUE(drain_decoded(fixture).empty()) << "a refused frame earns no ack";
}

// covers: SVR-4, SVR-5, SVR-6, REL-3
TEST(ServiceExchange, AppliesTheExchangeOnce)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> responses;
  SessionEvents events;
  events.service_response =
    [&responses](std::uint8_t req, std::uint8_t status, const std::vector<std::uint8_t> & payload) {
      responses.emplace_back(req, status, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 4u, {0xAAu}));
  drain_decoded(fixture);
  // The duplicate is acked again; the payload applies once.
  fixture.deliver_frame(kAck, 0u, {kHelperServices, 0x00u});
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcResponse, 0x00u, 0x04u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcResponse, 0x00u, 0x04u, 0x00u, 0x02u});
  ASSERT_EQ(responses.size(), 1u);
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 2u);
  EXPECT_EQ(units[0], with_check({kAck, 0u, kHelperServices, 0x00u}));
  EXPECT_EQ(units[1], with_check({kAck, 1u, kHelperServices, 0x00u}));
}

// covers: REL-2
TEST(ServiceExchange, RetriesThenGivesUp)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 4u, {}));
  drain_decoded(fixture);
  for (int retry = 0; retry < 3; ++retry) {
    fixture.advance_to(500u * static_cast<std::uint32_t>(retry + 1u));
    const auto units = drain_decoded(fixture);
    ASSERT_EQ(units.size(), 1u) << "retry " << retry;
    EXPECT_EQ(units[0][2], kHelperSvcRequest) << "the same rel seq goes out again";
    EXPECT_EQ(units[0][3], 0x00u);
  }
  // The link stays alive past the drop deadline: without MCU traffic the
  // link timeout at 2000 ms would re-handshake first.
  fixture.deliver_frame(kHeartbeat, 0u, {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u});
  fixture.advance_to(2500);
  for (const auto & unit : drain_decoded(fixture)) {
    EXPECT_NE(unit[0], kHelperServices) << "the spent REQUEST never goes out again";
  }
  EXPECT_EQ(fixture.session().reliable_dropped(), 1u);
}

// covers: REL-5
TEST(ServiceExchange, DropsServiceStateOnRehandshake)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> requests;
  SessionEvents events;
  events.service_request =
    [&requests](std::uint8_t index, std::uint8_t req, const std::vector<std::uint8_t> & payload) {
      requests.emplace_back(index, req, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 4u, {}));
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcRequest, 0x00u, 0x07u, 0x00u});
  drain_decoded(fixture);
  // A reset HELLO drops the outstanding REQUEST and the MCU's tracked call.
  fixture.deliver_frame(kControl, 1u, hello_payload(0x99u, 1u, 4u, 0x4Cu, 1000u));
  fixture.advance_to(2500);
  for (const auto & unit : drain_decoded(fixture)) {
    EXPECT_NE(unit[0], kHelperServices) << "no retry survives the handshake";
  }
  EXPECT_FALSE(fixture.session().send_service_response(7u, 0u, {}));
  EXPECT_EQ(requests.size(), 1u);
}

// covers: SVR-4
TEST(ServiceExchange, IgnoresAShortRequest)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> requests;
  SessionEvents events;
  events.service_request =
    [&requests](std::uint8_t index, std::uint8_t req, const std::vector<std::uint8_t> & payload) {
      requests.emplace_back(index, req, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  // A REQUEST too short for a call never surfaces: no ack, no event.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcRequest, 0x00u, 0x07u});
  EXPECT_TRUE(requests.empty());
  EXPECT_TRUE(drain_decoded(fixture).empty());
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

// covers: SVR-5
TEST(ServiceExchange, IgnoresShortAndStrayResponses)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> responses;
  SessionEvents events;
  events.service_response =
    [&responses](std::uint8_t req, std::uint8_t status, const std::vector<std::uint8_t> & payload) {
      responses.emplace_back(req, status, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  // A RESPONSE with no outstanding call is acked and dropped.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcResponse, 0x00u, 0x09u, 0x00u});
  EXPECT_TRUE(responses.empty());
  // A RESPONSE too short for an answer never reaches matching.
  fixture.deliver_frame(kHelperServices, 1u, {kHelperSvcResponse, 0x01u, 0x09u});
  EXPECT_TRUE(responses.empty());
  // The stream is not wedged: a real exchange still applies.
  EXPECT_TRUE(fixture.session().send_service_request(0u, 4u, {0x01u}));
  fixture.deliver_frame(kHelperServices, 2u, {kHelperSvcResponse, 0x01u, 0x04u, 0x00u, 0x02u});
  ASSERT_EQ(responses.size(), 1u);
  EXPECT_EQ(std::get<0>(responses[0]), 0x04u);
  EXPECT_EQ(std::get<1>(responses[0]), 0x00u);
  EXPECT_EQ(std::get<2>(responses[0]), std::vector<std::uint8_t>{0x02u});
}

// covers: SVR-5
TEST(ServiceExchange, DropsARequestForADeadService)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> requests;
  SessionEvents events;
  events.service_request =
    [&requests](std::uint8_t index, std::uint8_t req, const std::vector<std::uint8_t> & payload) {
      requests.emplace_back(index, req, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo & info) {
      return info.index == 0u ? kServiceStatusHashMismatch : kServiceStatusOk;
    }};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  fixture.deliver_frame(kHelperServices, 0u, svc_declare(0u, 0x00u, "reset"));
  fixture.deliver_frame(kHelperServices, 1u, svc_declare(1u, 0x00u, "trigger"));
  fixture.deliver_frame(kHelperServices, 2u, {kHelperSvcStatus, 0x00u, 0x02u, 0x01u});
  EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
  drain_decoded(fixture);
  // A REQUEST for the dead index matches nothing and is dropped.
  fixture.deliver_frame(kHelperServices, 3u, {kHelperSvcRequest, 0x00u, 0x07u, 0x00u});
  EXPECT_TRUE(requests.empty());
  // A host send to the dead index fails straight away.
  EXPECT_FALSE(fixture.session().send_service_request(0u, 4u, {}));
}

// covers: SVR-4
TEST(ServiceExchange, SendsBehindAPartialQueue)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  fixture.block_writes();
  // The first REQUEST stays queued on the blocked link; the queue holds
  // bytes below the frame cap, so the next send still goes through.
  EXPECT_TRUE(fixture.session().send_service_request(0u, 9u, {0xAAu}));
  EXPECT_TRUE(fixture.session().send_service_request(1u, 10u, {0xBBu}));
  // An MCU REQUEST still arrives over the blocked link, and its answer
  // queues behind the same partial queue.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcRequest, 0x00u, 0x07u, 0x00u});
  EXPECT_TRUE(fixture.session().send_service_response(7u, 0u, {0x01u}));
  fixture.allow_writes();
  fixture.advance_to(100u);
  const auto units = drain_decoded(fixture);
  ASSERT_FALSE(units.empty()) << "the queued frames go out once unblocked";
  EXPECT_EQ(units[0][2], kHelperSvcRequest);
}

// covers: SVR-4
TEST(ServiceExchange, RehandshakeDropsAQueuedServiceFrame)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  fixture.block_writes();
  // The REQUEST stays queued on the blocked link, fronting the queue.
  EXPECT_TRUE(fixture.session().send_service_request(0u, 9u, {0xAAu}));
  // A reset HELLO drops the queued frame with the rest of the state.
  fixture.deliver_frame(kControl, 1u, hello_payload(0x99u, 1u, 4u, 0x4Cu, 1000u));
  EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
  fixture.advance_to(2500);
  for (const auto & unit : drain_decoded(fixture)) {
    EXPECT_NE(unit[0], kHelperServices) << "no queued service frame survives";
  }
}

// covers: SVR-1, SVR-7
TEST(ServiceWalk, IgnoresADeclareWithAShortHash)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  const auto walked = drain_decoded(fixture);
  ASSERT_FALSE(walked.empty());
  EXPECT_EQ(walked.back()[2], kHelperSvcQuery);
  EXPECT_EQ(walked.back()[3], 0x00u);
  // Three hash bytes where eight belong: no entry is kept, so the verdict
  // is unknown and the walk moves on.
  fixture.deliver_frame(kHelperServices, 0u,
    {kHelperSvcDeclare, 0x00u, 0x00u, 0x01u, 0x02u, 0x03u});
  const auto next = drain_decoded(fixture);
  ASSERT_EQ(next.size(), 2u);
  EXPECT_EQ(next[0][2], kHelperSvcStatus);
  EXPECT_EQ(next[0][4], 0x00u);
  EXPECT_EQ(next[0][5], 0x01u);
  EXPECT_EQ(next[1][2], kHelperSvcQuery);
  EXPECT_EQ(next[1][3], 0x01u);
}

// covers: SVR-7
TEST(ServiceWalk, IgnoresAShortStatus)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  const auto walked = drain_decoded(fixture);
  ASSERT_FALSE(walked.empty());
  EXPECT_EQ(walked.back()[2], kHelperSvcQuery);
  EXPECT_EQ(walked.back()[3], 0x00u);
  // Two bytes where four belong: the walk does not move.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u});
  EXPECT_TRUE(drain_decoded(fixture).empty());
  fixture.deliver_frame(kHelperServices, 1u, svc_declare(0u, 0x00u, "reset"));
  const auto next = drain_decoded(fixture);
  ASSERT_EQ(next.size(), 2u);
  EXPECT_EQ(next[0][2], kHelperSvcStatus);
  EXPECT_EQ(next[0][5], 0x00u);
  EXPECT_EQ(next[1][2], kHelperSvcQuery);
  EXPECT_EQ(next[1][3], 0x01u);
}

// covers: SVR-7
TEST(ServiceWalk, IgnoresAStatusForAnotherIndex)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  const auto walked = drain_decoded(fixture);
  ASSERT_FALSE(walked.empty());
  EXPECT_EQ(walked.back()[2], kHelperSvcQuery);
  EXPECT_EQ(walked.back()[3], 0x00u);
  // The verdict names index 9 while index 0 is under request.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x09u, 0x01u});
  EXPECT_TRUE(drain_decoded(fixture).empty());
  fixture.deliver_frame(kHelperServices, 1u, svc_declare(0u, 0x00u, "reset"));
  const auto next = drain_decoded(fixture);
  ASSERT_EQ(next.size(), 2u);
  EXPECT_EQ(next[0][5], 0x00u);
  EXPECT_EQ(next[1][3], 0x01u);
}

// covers: SVR-7
TEST(ServiceWalk, AdvancesPastAZeroVerdict)
{
  SessionConfig config;
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  fixture.deliver_frame(kControl, 0u, hello_payload(0x44u, 1u, 4u, 0x4Cu, 1000u));
  fixture.deliver_frame(kControl, 1u, topic_payload(0u, 0x10u, "battery"));
  fixture.deliver_frame(kParams, 0u, {kParamStatus, 0x00u, 0x00u, 0x01u});
  const auto walked = drain_decoded(fixture);
  ASSERT_FALSE(walked.empty());
  EXPECT_EQ(walked.back()[2], kHelperSvcQuery);
  EXPECT_EQ(walked.back()[3], 0x00u);
  // Only verdict 1 ends the walk at the index under request.
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcStatus, 0x00u, 0x00u, 0x00u});
  const auto next = drain_decoded(fixture);
  ASSERT_EQ(next.size(), 1u);
  EXPECT_EQ(next[0][2], kHelperSvcQuery);
  EXPECT_EQ(next[0][3], 0x01u);
}

// covers: SVR-1, SVR-7
TEST(ServiceWalk, IgnoresADeclareWhileConnected)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperServices, 3u, svc_declare(0u, 0x00u, "reset"));
  EXPECT_TRUE(drain_decoded(fixture).empty());
}

// covers: SVR-4
TEST(ServiceExchange, IgnoresAnEmptyServiceFrame)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperServices, 3u, {});
  EXPECT_TRUE(drain_decoded(fixture).empty());
}

// covers: SVR-4
TEST(ServiceExchange, DropsARequestWithAWrongRelSeq)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> requests;
  SessionEvents events;
  events.service_request =
    [&requests](std::uint8_t index, std::uint8_t req, const std::vector<std::uint8_t> & payload) {
      requests.emplace_back(index, req, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  drain_decoded(fixture);
  // Rel 5 ahead of the expected rel 0: acked, never applied.
  fixture.deliver_frame(kHelperServices, 3u, {kHelperSvcRequest, 0x05u, 0x07u, 0x00u});
  EXPECT_TRUE(requests.empty());
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 1u);
  EXPECT_EQ(units[0], with_check({kAck, 0u, kHelperServices, 0x05u}));
}

// covers: SVR-4
TEST(ServiceExchange, IgnoresAnUnknownServiceOpcode)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperServices, 3u, {0x09u, 0x00u, 0x00u, 0x00u});
  EXPECT_TRUE(drain_decoded(fixture).empty());
}

// covers: REL-2
TEST(ServiceExchange, HoldsTheRetryWhileTheLinkIsWedged)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 4u, {}));
  drain_decoded(fixture);
  fixture.block_writes();
  EXPECT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(8u, 0xAAu)));
  // The deadline passes with the queue full: no retry spends itself.
  fixture.advance_to(600);
  EXPECT_TRUE(drain_decoded(fixture).empty());
  // The wedge lifts: the queued user frame flushes, then the retry goes
  // out on the same rel seq. One window only: the retry re-arms its own
  // deadline, and a second window would spend it again.
  fixture.allow_writes();
  fixture.advance_to(700);
  const auto units = drain_decoded(fixture);
  ASSERT_EQ(units.size(), 2u);
  EXPECT_EQ(units[0][0], 0x10u) << "the queued user frame flushes first";
  EXPECT_EQ(units[1][2], kHelperSvcRequest);
  EXPECT_EQ(units[1][3], 0x00u);
}

// covers: SVR-4
TEST(ServiceExchange, RefusesToSendForAnUnknownService)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_FALSE(fixture.session().send_service_request(9u, 4u, {}));
}

// covers: SVR-4
TEST(ServiceExchange, RefusesToSendPastTheLinkCapacity)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  // The handshake caps the frame at 515 bytes.
  EXPECT_FALSE(fixture.session().send_service_request(
    0u, 4u, std::vector<std::uint8_t>(1100u, 0xAAu)));
}

// covers: SVR-4
TEST(ServiceExchange, RefusesToSendWhileTheQueueIsFull)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  drain_decoded(fixture);
  fixture.block_writes();
  EXPECT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(1100u, 0xAAu)));
  EXPECT_FALSE(fixture.session().send_service_request(0u, 4u, {}));
}

// covers: SVR-5
TEST(ServiceExchange, RefusesAnOversizeAnswer)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperServices, 3u, {kHelperSvcRequest, 0x00u, 0x07u, 0x01u});
  // The handshake caps the frame at 515 bytes.
  EXPECT_FALSE(fixture.session().send_service_response(
    7u, 0u, std::vector<std::uint8_t>(1100u, 0xAAu)));
}

// covers: SVR-5
TEST(ServiceExchange, RefusesAnAnswerWhileTheQueueIsFull)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  drain_decoded(fixture);
  fixture.deliver_frame(kHelperServices, 3u, {kHelperSvcRequest, 0x00u, 0x07u, 0x01u});
  fixture.block_writes();
  EXPECT_TRUE(fixture.session().send_user(0x10u, std::vector<std::uint8_t>(1100u, 0xAAu)));
  EXPECT_FALSE(fixture.session().send_service_response(7u, 0u, {0x01u}));
}

// covers: SVR-5, SVR-10
TEST(ServiceExchange, ReplacesAPendingAnswer)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, SessionEvents{},
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 1u, {}));
  fixture.deliver_frame(kHelperServices, 3u, {kHelperSvcRequest, 0x00u, 0x07u, 0x01u});
  drain_decoded(fixture);
  // The stream is busy, so both answers queue and the newer replaces.
  EXPECT_TRUE(fixture.session().send_service_response(7u, 0u, {0x01u}));
  EXPECT_TRUE(fixture.session().send_service_response(7u, 0u, {0x02u}));
  EXPECT_EQ(fixture.session().reliable_replaced(), 1u);
  // The ack frees the stream: the replacing answer goes out.
  fixture.deliver_frame(kAck, 0u, {kHelperServices, 0x00u});
  const auto answered = drain_decoded(fixture);
  ASSERT_EQ(answered.size(), 1u);
  EXPECT_EQ(answered[0],
    with_check({kHelperServices, 1u, kHelperSvcResponse, 0x01u, 0x07u, 0x00u, 0x02u}));
}

// covers: SVR-5, SVR-10
TEST(ServiceExchange, PrioritizesTheAnswerOverQueuedRequests)
{
  SessionConfig config = quiet_config();
  config.time_sync_enabled = false;
  std::vector<std::tuple<std::uint8_t, std::uint8_t, std::vector<std::uint8_t>>> requests;
  SessionEvents events;
  events.service_request =
    [&requests](std::uint8_t index, std::uint8_t req, const std::vector<std::uint8_t> & payload) {
      requests.emplace_back(index, req, payload);
    };
  Fixture fixture{
    [](const TopicInfo &) {return std::uint8_t{0};}, config, std::move(events),
    [](const ServiceInfo &) {return kServiceStatusOk;}};
  handshake_with_services(fixture);
  EXPECT_TRUE(fixture.session().send_service_request(0u, 1u, {}));
  fixture.deliver_frame(kHelperServices, 0u, {kHelperSvcRequest, 0x00u, 0x07u, 0x00u});
  ASSERT_EQ(requests.size(), 1u);
  // The stream is busy, so both the answer and a second call queue.
  EXPECT_TRUE(fixture.session().send_service_response(7u, 0u, {0x01u}));
  EXPECT_TRUE(fixture.session().send_service_request(1u, 2u, {}));
  const auto queued = drain_decoded(fixture);
  ASSERT_EQ(queued.size(), 2u);
  EXPECT_EQ(queued[0], with_check({kHelperServices, 0u, kHelperSvcRequest, 0x00u, 0x01u, 0x00u}));
  EXPECT_EQ(queued[1], with_check({kAck, 0u, kHelperServices, 0x00u}));
  // The ack frees the stream: the answer goes out before the queued call,
  // each with the next rel seq.
  fixture.deliver_frame(kAck, 0u, {kHelperServices, 0x00u});
  const auto answered = drain_decoded(fixture);
  ASSERT_EQ(answered.size(), 1u);
  EXPECT_EQ(
    answered[0],
    with_check({kHelperServices, 1u, kHelperSvcResponse, 0x01u, 0x07u, 0x00u, 0x01u}));
  fixture.deliver_frame(kAck, 1u, {kHelperServices, 0x01u});
  const auto next = drain_decoded(fixture);
  ASSERT_EQ(next.size(), 1u);
  EXPECT_EQ(next[0], with_check({kHelperServices, 2u, kHelperSvcRequest, 0x02u, 0x02u, 0x01u}));
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
