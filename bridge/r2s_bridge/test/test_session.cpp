// The host session: the transcript vectors replayed step by step, plus the
// deadlines, retries and reset rules that a transcript cannot pin to a
// millisecond. Time is the manual clock's alone.
// covers: SES-12, SES-13, SES-14, SES-15, SES-16, SES-17, SES-18, SES-19, SES-20, SES-21, CTL-3, CTL-9
// covers: CTL-13, SES-32
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "fake_transport.hpp"
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

std::vector<std::uint8_t> bytes_of(const std::string &hex)
{
    std::vector<std::uint8_t> bytes;
    std::istringstream in(hex);
    std::string pair;
    while (in >> pair) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16)));
    }
    return bytes;
}

std::string hex_of(const std::vector<std::uint8_t> &bytes)
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
std::vector<std::uint8_t> wire_of(const std::vector<std::uint8_t> &decoded)
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
std::vector<std::uint8_t> with_topic_frame(const std::vector<std::uint8_t> &payload)
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

std::vector<std::vector<std::uint8_t>> units_of(const std::vector<std::uint8_t> &bytes)
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
// MCU announces.
std::vector<std::uint8_t> hello_payload(std::uint32_t boot_id, std::uint8_t topics, std::uint8_t version = 1u,
                                        std::uint8_t features = 0u, std::uint32_t tick_hz = 1000000u)
{
    std::vector<std::uint8_t> p{0x02u, version, 0x00u};
    const auto put_le = [&p](std::uint32_t value, int bytes) {
        for (int i = 0; i < bytes; ++i) {
            p.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
        }
    };
    put_le(boot_id, 4);
    put_le(512, 2); // max_frame
    put_le(515, 2); // rx_capacity
    put_le(tick_hz, 4);
    p.push_back(features);
    p.push_back(topics);
    const std::string name = "node";
    p.push_back(static_cast<std::uint8_t>(name.size()));
    p.insert(p.end(), name.begin(), name.end());
    return p;
}

std::vector<std::uint8_t> topic_payload(std::uint8_t index, std::uint8_t topic_id, const std::string &name,
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
    explicit Link(FakeTransport &inner) : inner_(inner)
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

// A session over the test's own fake link, driven by the test's clock only.
class Fixture
{
  public:
    explicit Fixture(
        TopicResolver resolve = [](const TopicInfo &) { return std::uint8_t{0}; },
        SessionConfig config = SessionConfig())
    {
        session_ = std::make_unique<Session>(
            config, clock_, [this]() { return std::unique_ptr<Transport>(new Link(transport_)); },
            std::move(resolve));
    }

    // Moves time one tick at a time, polling after each, so a timer fires on
    // the first tick at which its deadline has passed.
    void advance_to(std::uint32_t ms)
    {
        while (now_ms_ < ms) {
            clock_.advance(kTick);
            ++now_ms_;
            session_->poll();
        }
    }

    void deliver(const std::vector<std::uint8_t> &decoded)
    {
        transport_.script_read(wire_of(decoded));
        session_->poll();
    }

    // Delivers one decoded frame: the caller passes topic, seq and payload,
    // and the check is appended here, as [CHK-3] lays it out.
    void deliver_frame(std::uint8_t topic_id, std::uint8_t seq, const std::vector<std::uint8_t> &payload)
    {
        std::vector<std::uint8_t> decoded{topic_id, seq};
        decoded.insert(decoded.end(), payload.begin(), payload.end());
        const std::uint16_t crc =
            r2s_crc16_update_buf(R2S_CRC16_INIT, decoded.data(), static_cast<std::uint16_t>(decoded.size()));
        decoded.push_back(static_cast<std::uint8_t>(crc & 0xFFu));
        decoded.push_back(static_cast<std::uint8_t>(crc >> 8u));
        deliver(decoded);
    }

    // The host frames written since the last drain.
    std::vector<std::vector<std::uint8_t>> drain_units()
    {
        std::vector<std::uint8_t> all;
        for (const auto &write : transport_.writes()) {
            all.insert(all.end(), write.begin(), write.end());
        }
        transport_.discard_writes();
        return units_of(all);
    }

    Session &session()
    {
        return *session_;
    }
    std::uint32_t now_ms() const
    {
        return now_ms_;
    }

  private:
    ManualClock clock_;
    FakeTransport transport_;
    std::uint32_t now_ms_ = 0;
    std::unique_ptr<Session> session_;
};

struct Step {
    bool from_host = false;
    std::vector<std::uint8_t> decoded;
    std::uint32_t at_ms = 0;
};

std::vector<std::string> transcript_vectors()
{
    std::vector<std::string> files;
    for (const auto &entry : std::filesystem::directory_iterator(R2S_SESSION_VECTOR_DIR)) {
        if (entry.path().extension() == ".yaml") {
            files.push_back(entry.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

class SessionTranscript : public ::testing::TestWithParam<std::string>
{
};

// Every step arrives at its own time, and the session answers with exactly
// the transcript's host frames, in order.
TEST_P(SessionTranscript, ReplaysTheTranscriptInOrder)
{
    const YAML::Node root = YAML::LoadFile(GetParam());
    const std::string name = root["name"].as<std::string>();
    std::vector<Step> steps;
    for (const auto &node : root["steps"]) {
        Step step;
        step.from_host = node["from"].as<std::string>() == "host";
        step.decoded = bytes_of(node["frame"].as<std::string>());
        step.at_ms = node["at_ms"].as<std::uint32_t>();
        steps.push_back(std::move(step));
    }
    ASSERT_FALSE(steps.empty()) << name;

    Fixture fixture;
    std::vector<std::size_t> expected;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].from_host) {
            expected.push_back(i);
        }
    }

    std::size_t next = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const Step &step = steps[i];
        SCOPED_TRACE(name + " step " + std::to_string(i));
        fixture.advance_to(step.at_ms);
        if (!step.from_host) {
            fixture.deliver(step.decoded);
        }
        const std::vector<std::vector<std::uint8_t>> written = fixture.drain_units();
        for (const auto &unit : written) {
            ASSERT_LT(next, expected.size()) << name << ": the session wrote more than the transcript";
            const Step &want = steps[expected[next]];
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
        for (const auto &unit : fixture.drain_units()) {
            ASSERT_LT(next, expected.size()) << name << ": the session wrote more than the transcript";
            EXPECT_EQ(hex_of(unit), hex_of(wire_of(steps[expected[next]].decoded)))
                << name << " host step " << expected[next];
            ++next;
        }
    }
    EXPECT_EQ(next, expected.size()) << name << ": the transcript's host frames did not all go out";
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
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    EXPECT_EQ(fixture.session().boot_id(), 0x1000u);
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
    events.log = [&logged](SessionLog, const std::string &text) { logged.push_back(text); };
    ManualClock clock;
    FakeTransport transport;
    Session session(
        SessionConfig{}, clock, [&transport]() { return std::unique_ptr<Transport>(new Link(transport)); },
        [](const TopicInfo &) { return std::uint8_t{0}; }, events);
    const std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u, 2u);
    std::vector<std::uint8_t> frame{kControl, 0u};
    frame.insert(frame.end(), hello.begin(), hello.end());
    transport.script_read(wire_of(with_check(frame)));
    session.poll();
    EXPECT_EQ(session.state(), SessionState::kBootWait) << "a refused HELLO changes nothing";
    ASSERT_EQ(logged.size(), 1u);
    EXPECT_NE(logged[0].find("MCU protocol version is 2"), std::string::npos)
        << "the error names the MCU version";
    EXPECT_NE(logged[0].find("host protocol version is 1"), std::string::npos)
        << "the error names the host version";
}

TEST(SessionHandshake, RefusesAFeaturesByteWithAReservedBit)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 1u, 0x40u));
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().has_boot_id());
}

TEST(SessionHandshake, AcceptsAnMcuWhoseDoubleIs32Bits)
{
    Fixture fixture;
    fixture.advance_to(10);
    // Bit 4 is informational, and every AVR build sets it ([CTL-2]).
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 1u, 0x10u));
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
    EXPECT_EQ(fixture.session().boot_id(), 0x44332211u);
}

TEST(SessionHandshake, RefusesATickRateThatIsNotThousandTimesADivisor)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 1u, 0u, 1500u));
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u, 1u, 0u, 1000u));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected) << "1 kHz is a valid form";
}

TEST(SessionHandshake, SendsReadyAfterTheLastTopic)
{
    Fixture fixture;
    fixture.advance_to(10);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 1u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 0u, topic_payload(0u, 0x10u, "battery"));
    const auto units = fixture.drain_units();
    ASSERT_EQ(units.size(), 2u) << "the TOPIC_STATUS and then READY";
    EXPECT_EQ(units[1][2], 0x06u);
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
    ASSERT_EQ(fixture.session().topics().size(), 1u);
    EXPECT_EQ(fixture.session().topics()[0].topic_name, "battery");
}

TEST(SessionHandshake, RejectsOneTopicAndKeepsTheOthers)
{
    Fixture fixture([](const TopicInfo &info) {
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
    Fixture fixture([](const TopicInfo &) { return kTopicStatusUnknownType; });
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
    events.handshake_completed = [&identical](bool same) { identical.push_back(same); };
    ManualClock clock;
    FakeTransport transport;
    std::uint32_t now = 0;
    Session session(
        SessionConfig{}, clock, [&transport]() { return std::unique_ptr<Transport>(new Link(transport)); },
        [](const TopicInfo &) { return std::uint8_t{0}; }, events);

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
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected) << "user traffic is host traffic too";
}

TEST(SessionConnected, ResetsOnAHelloWithTheSameBootId)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    ASSERT_EQ(fixture.session().state(), SessionState::kConnected);
    fixture.drain_units();
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected)
        << "an empty table resolves at once, so the re-handshake is already done";
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "READY for the new handshake";
}

TEST(SessionConnected, ResetsOnAHelloWithANewBootId)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    fixture.drain_units();
    fixture.deliver_frame(kControl, 0u, hello_payload(0x2000u, 1u));
    EXPECT_EQ(fixture.session().state(), SessionState::kHandshaking);
    EXPECT_EQ(fixture.session().boot_id(), 0x2000u);
    EXPECT_EQ(fixture.drain_units().size(), 1u) << "the TOPIC_REQ of the new table";
}

TEST(SessionConnected, RecordsTheMcuCountersFromAHeartbeat)
{
    Fixture fixture;
    fixture.advance_to(100);
    fixture.deliver_frame(kControl, 0u, hello_payload(0x1000u, 0u));
    fixture.deliver_frame(kHeartbeat, 0u, std::vector<std::uint8_t>{1u, 0u, 2u, 0u, 3u, 0u, 4u, 0u});
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
        EXPECT_TRUE(fixture.drain_units().empty()) << "nothing but HELLO_REQ at 500 ms, got a frame at " << t;
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
    EXPECT_TRUE(fixture.drain_units().empty()) << "the abandoned walk sends nothing after the restart";
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
        SessionConfig{}, clock,
        [&opens, &first, &second]() -> std::unique_ptr<Transport> {
            ++opens;
            return std::unique_ptr<Transport>(new Link(opens == 1 ? first : second));
        },
        [](const TopicInfo &) { return std::uint8_t{0}; });
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
        for (const auto &write : second.writes()) {
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
    Fixture fixture([](const TopicInfo &) { return std::uint8_t{0}; }, config);
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
    hello[7] = 0xFEu; // max_frame low byte: 65278, one above 65277
    hello[8] = 0xFEu;
    fixture.deliver_frame(kControl, 0u, hello);
    EXPECT_EQ(fixture.session().state(), SessionState::kBootWait);
    EXPECT_FALSE(fixture.session().has_boot_id());
}

TEST(SessionHandshake, RefusesACompactHandshake)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    hello[2] = 0x01u; // the handshake mode byte ([CTL-1])
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
    EXPECT_FALSE(fixture.session().has_boot_id());
}

TEST(SessionHandshake, RefusesANodeNameWithBytesLeftOver)
{
    Fixture fixture;
    fixture.advance_to(10);
    std::vector<std::uint8_t> hello = hello_payload(0x1000u, 0u);
    hello.push_back(0x00u); // a byte after the name
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
        SessionConfig{}, clock,
        [&opens, &transport]() -> std::unique_ptr<Transport> {
            ++opens;
            if (opens == 1) {
                return nullptr;
            }
            return std::unique_ptr<Transport>(new Link(transport));
        },
        [](const TopicInfo &) { return std::uint8_t{0}; });
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
        explicit FailingWrites(FakeTransport &inner) : inner_(inner)
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
        FakeTransport &inner_;
    };
    Session session(
        SessionConfig{}, clock,
        [&incoming]() { return std::unique_ptr<Transport>(new FailingWrites(incoming)); },
        [](const TopicInfo &) { return std::uint8_t{0}; });
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
        SessionConfig{}, clock, [&transport]() { return std::unique_ptr<Transport>(new Link(transport)); },
        [](const TopicInfo &) { return std::uint8_t{0}; });
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
                     config, clock, []() { return std::unique_ptr<Transport>(); },
                     [](const TopicInfo &) { return std::uint8_t{0}; }),
                 std::invalid_argument);
}
// A decoded wire unit back to its frame, through the stateless decoder.
std::vector<std::uint8_t> decoded_unit(std::vector<std::uint8_t> unit)
{
    unit.pop_back();
    std::uint8_t *at = nullptr;
    std::uint16_t len = 0;
    if (r2s_rcobs_decode(unit.data(), static_cast<std::uint16_t>(unit.size()), &at, &len) != R2S_RCOBS_OK) {
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
    for (const auto &unit : fixture.drain_units()) {
        const std::vector<std::uint8_t> decoded = decoded_unit(unit);
        if (decoded.size() >= 5u && decoded[0] == kControl && decoded[2] == 0x05u && decoded[3] == 17u &&
            decoded[4] == kTopicStatusNameConflict) {
            refused = true;
        }
    }
    EXPECT_TRUE(refused) << "the second TOPIC_STATUS carries the conflict verdict";
    ASSERT_EQ(fixture.session().topics().size(), 2u);
    EXPECT_EQ(fixture.session().topics()[0].status, kTopicStatusOk);
    EXPECT_EQ(fixture.session().topics()[1].status, kTopicStatusNameConflict);
    EXPECT_EQ(fixture.session().state(), SessionState::kConnected);
}

TEST(SessionSend, RefusesFramesPastTheMcuCapacity)
{
    ManualClock clock;
    FakeTransport transport;
    Session session(
        SessionConfig{}, clock, [&transport]() { return std::unique_ptr<Transport>(new Link(transport)); },
        [](const TopicInfo &) { return std::uint8_t{0}; });
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

} // namespace
} // namespace test
} // namespace r2s_bridge
