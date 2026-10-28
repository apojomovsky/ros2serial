// The bridge diagnostics: link state, session token, resets, protocol
// version, MCU counters, per-topic gaps and drops, and the time sync slot,
// through a real node with a scripted link and real type support.
// covers: FRM-15, HB-2, SES-15, SES-16, SES-19, SES-21, SES-26, SES-30
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <geometry_msgs/msg/twist.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float32.hpp>

#include "frame_helpers.hpp"
#include "manual_clock.hpp"
#include "r2s_bridge/router.hpp"
#include "rosidl_runtime_c/type_hash.h"
extern "C" {
#include "r2s/r2s.h"
}

namespace r2s_bridge
{
namespace test
{
namespace
{

constexpr std::uint8_t kBattery = 16u;
constexpr std::uint8_t kCmdVel = 17u;
constexpr std::uint8_t kHeartbeat = 4u;
using Diagnostics = Router::Diagnostics;
using TopicDiagnostics = Router::TopicDiagnostics;

std::array<std::uint8_t, 8> installed_hash(const std::string &type)
{
    const auto library = rclcpp::get_typesupport_library(type, "rosidl_typesupport_cpp");
    const rosidl_message_type_support_t *handle =
        rclcpp::get_message_typesupport_handle(type, "rosidl_typesupport_cpp", *library);
    const rosidl_type_hash_t *hash = handle->get_type_hash_func(handle);
    std::array<std::uint8_t, 8> out{};
    std::copy(hash->value, hash->value + 8u, out.begin());
    return out;
}

std::vector<std::uint8_t> heartbeat_payload(std::uint16_t rx, std::uint16_t crc, std::uint16_t overflow,
                                            std::uint16_t dropped)
{
    const std::uint16_t fields[] = {rx, crc, overflow, dropped};
    std::vector<std::uint8_t> out;
    for (const std::uint16_t field : fields) {
        out.push_back(static_cast<std::uint8_t>(field & 0xFFu));
        out.push_back(static_cast<std::uint8_t>(field >> 8u));
    }
    return out;
}

class DiagnosticsFixture
{
  public:
    DiagnosticsFixture(const std::string &name, RouterParams params, TransportFactory open = {})
        : node_(std::make_shared<rclcpp::Node>(name))
    {
        if (!open) {
            open = [this]() { return std::unique_ptr<Transport>(new HelperLink(transport_)); };
        }
        router_ = std::make_unique<Router>(
            node_->get_node_topics_interface(), std::move(open), clock_, std::move(params),
            [this](SessionLog, const std::string &text) { logged_.push_back(text); });
    }

    void advance(std::uint32_t ms)
    {
        clock_.advance(std::chrono::milliseconds(ms));
        router_->poll();
    }
    // Discovery runs on background threads, so an endpoint created an instant
    // ago may not be matched yet. The wait polls the condition with a
    // deadline and returns as soon as it holds.
    template <typename Ready> void wait_until(Ready ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!ready() && std::chrono::steady_clock::now() < deadline) {
            rclcpp::spin_some(node_);
        }
    }

    void script_closed()
    {
        transport_.script_read(IoStatus::kClosed);
    }

    void poll_once()
    {
        router_->poll();
    }

    void deliver_frame(std::uint8_t topic_id, std::uint8_t seq, const std::vector<std::uint8_t> &payload)
    {
        std::vector<std::uint8_t> decoded{topic_id, seq};
        decoded.insert(decoded.end(), payload.begin(), payload.end());
        transport_.script_read(helper_wire_of(helper_with_check(decoded)));
        router_->poll();
    }

    // flags bit 0 set means the MCU subscribes (host to MCU); bit 2 selects
    // the keep-latest queue.
    void handshake_full(std::uint32_t token, std::uint8_t cmd_vel_flags)
    {
        deliver_frame(kHelperControl, 0u, helper_hello(token, 2u));
        router_->poll();
        const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
        deliver_frame(
            kHelperControl, 1u,
            helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
        router_->poll();
        const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
        deliver_frame(kHelperControl, 2u,
                      helper_topic(1u, kCmdVel, cmd_vel_flags, twist_hash.data(), "geometry_msgs/msg/Twist",
                                   "cmd_vel"));
        router_->poll();
        ASSERT_EQ(router_->publisher_count(), 1u);
        ASSERT_EQ(router_->subscription_count(), 1u);
    }

    void handshake(std::uint32_t token = 7u)
    {
        handshake_full(token, 0x01u);
    }

    Diagnostics snapshot() const
    {
        return router_->diagnostics();
    }

    Router &router()
    {
        return *router_;
    }
    rclcpp::Node::SharedPtr node()
    {
        return node_;
    }

  private:
    FakeTransport transport_;
    ManualClock clock_;
    rclcpp::Node::SharedPtr node_;
    std::unique_ptr<Router> router_;
    std::vector<std::string> logged_;
};

class DiagnosticsNode : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        rclcpp::init(0, nullptr);
    }
    static void TearDownTestSuite()
    {
        rclcpp::shutdown();
    }
};

const TopicDiagnostics *find_topic(const Diagnostics &diagnostics, std::uint8_t topic_id)
{
    for (const TopicDiagnostics &topic : diagnostics.topics) {
        if (topic.topic_id == topic_id) {
            return &topic;
        }
    }
    return nullptr;
}

TEST_F(DiagnosticsNode, ReportsBootWaitBeforeAnyHello)
{
    DiagnosticsFixture fixture("boot", RouterParams{});
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "boot_wait");
    EXPECT_EQ(diagnostics.session_token, 0u);
    EXPECT_EQ(diagnostics.resets, 0u);
    EXPECT_TRUE(diagnostics.topics.empty());
    EXPECT_EQ(diagnostics.mcu_counters.rx_frames, 0u);
    EXPECT_FALSE(diagnostics.time_sync_available);
}

TEST_F(DiagnosticsNode, ReportsDisconnectedWhenThePortNeverOpens)
{
    DiagnosticsFixture fixture("down", RouterParams{},
                               []() -> std::unique_ptr<Transport> { return nullptr; });
    EXPECT_EQ(fixture.snapshot().link_state, "disconnected");
}

TEST_F(DiagnosticsNode, ReportsConnectedWithSessionTokenAfterHandshake)
{
    DiagnosticsFixture fixture("up", RouterParams{});
    fixture.handshake();
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "connected");
    EXPECT_EQ(diagnostics.session_token, 1u) << "the first READY assigns token 1";
    EXPECT_EQ(diagnostics.resets, 0u);
    EXPECT_EQ(diagnostics.protocol_version, R2S_PROTOCOL_VERSION);
    ASSERT_EQ(diagnostics.topics.size(), 2u);
    const TopicDiagnostics *battery = find_topic(diagnostics, kBattery);
    ASSERT_NE(battery, nullptr);
    EXPECT_EQ(battery->topic_name, "battery");
    EXPECT_TRUE(battery->device_to_host);
    const TopicDiagnostics *cmd_vel = find_topic(diagnostics, kCmdVel);
    ASSERT_NE(cmd_vel, nullptr);
    EXPECT_EQ(cmd_vel->topic_name, "cmd_vel");
    EXPECT_FALSE(cmd_vel->device_to_host);
}

TEST_F(DiagnosticsNode, ReportsMcuCountersFromHeartbeat)
{
    DiagnosticsFixture fixture("hb", RouterParams{});
    fixture.handshake();
    fixture.deliver_frame(kHeartbeat, 0u, heartbeat_payload(10u, 2u, 1u, 3u));
    fixture.deliver_frame(kHeartbeat, 5u, heartbeat_payload(11u, 2u, 1u, 4u));
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.mcu_counters.rx_frames, 11u);
    EXPECT_EQ(diagnostics.mcu_counters.crc_errors, 2u);
    EXPECT_EQ(diagnostics.mcu_counters.rx_overflows, 1u);
    EXPECT_EQ(diagnostics.mcu_counters.dropped_frames, 4u);
}
TEST_F(DiagnosticsNode, CountsFirstFrameSeqAsGaps)
{
    DiagnosticsFixture fixture("firstgap", RouterParams{});
    fixture.handshake();
    const std::vector<std::uint8_t> one{0x00, 0x00, 0x80, 0x3F};
    fixture.deliver_frame(kBattery, 3u, one);
    const TopicDiagnostics *battery = find_topic(fixture.snapshot(), kBattery);
    ASSERT_NE(battery, nullptr);
    EXPECT_EQ(battery->seq_gaps, 3u) << "the first frame after a handshake expects seq 0";
}

TEST_F(DiagnosticsNode, CountsSkippedMcuFramesAsGaps)
{
    DiagnosticsFixture fixture("gaps", RouterParams{});
    fixture.handshake();
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "battery", rclcpp::QoS(10).best_effort(),
        [&received](const std_msgs::msg::Float32 &msg) { received.push_back(msg.data); });
    fixture.wait_until([&sub]() { return sub->get_publisher_count() > 0u; });
    const std::vector<std::uint8_t> one{0x00, 0x00, 0x80, 0x3F};
    fixture.deliver_frame(kBattery, 0u, one);
    fixture.deliver_frame(kBattery, 1u, one);
    fixture.deliver_frame(kBattery, 4u, one);
    // Delivery runs on background threads, so the test waits for every
    // sample instead of assuming one spin is enough.
    fixture.wait_until([&received]() { return received.size() >= 3u; });
    const Diagnostics diagnostics = fixture.snapshot();
    const TopicDiagnostics *battery = find_topic(diagnostics, kBattery);
    ASSERT_NE(battery, nullptr);
    EXPECT_EQ(battery->seq_gaps, 2u) << "seq 2 and 3 never arrived";
    EXPECT_EQ(received.size(), 3u) << "a gap is not an error and does not drop the frame";
}

TEST_F(DiagnosticsNode, RestartsGapCountAfterRehandshake)
{
    DiagnosticsFixture fixture("regap", RouterParams{});
    fixture.handshake();
    const std::vector<std::uint8_t> one{0x00, 0x00, 0x80, 0x3F};
    fixture.deliver_frame(kBattery, 0u, one);
    fixture.deliver_frame(kBattery, 2u, one);
    EXPECT_EQ(find_topic(fixture.snapshot(), kBattery)->seq_gaps, 1u);
    fixture.handshake(8u);
    fixture.deliver_frame(kBattery, 0u, one);
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "connected");
    EXPECT_EQ(find_topic(diagnostics, kBattery)->seq_gaps, 1u)
        << "seq restarts at 0 after each handshake, so 0 arrives as expected";
}

TEST_F(DiagnosticsNode, CountsHelloWhileConnectedAsReset)
{
    DiagnosticsFixture fixture("reset", RouterParams{});
    fixture.handshake();
    EXPECT_EQ(fixture.snapshot().session_token, 1u);
    fixture.deliver_frame(kHelperControl, 9u, helper_hello(0u, 2u));
    Diagnostics mid = fixture.snapshot();
    EXPECT_EQ(mid.link_state, "handshaking");
    EXPECT_EQ(mid.resets, 1u);
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    fixture.deliver_frame(
        kHelperControl, 10u,
        helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
    const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
    fixture.deliver_frame(
        kHelperControl, 11u,
        helper_topic(1u, kCmdVel, 0x01u, twist_hash.data(), "geometry_msgs/msg/Twist", "cmd_vel"));
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "connected");
    EXPECT_EQ(diagnostics.resets, 1u);
    EXPECT_EQ(diagnostics.session_token, 2u) << "the new handshake assigns a fresh token";
}
TEST_F(DiagnosticsNode, RefusedHelloKeepsBootWait)
{
    DiagnosticsFixture fixture("badhello", RouterParams{});
    EXPECT_EQ(fixture.snapshot().link_state, "boot_wait");
    std::vector<std::uint8_t> bad_version = helper_hello(7u, 2u);
    bad_version[1] = 0xFFu;
    fixture.deliver_frame(kHelperControl, 0u, bad_version);
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "boot_wait") << "a refused HELLO changes nothing";
    EXPECT_EQ(diagnostics.session_token, 0u);
    EXPECT_EQ(diagnostics.resets, 0u);
}
TEST_F(DiagnosticsNode, LinkTimeoutRehandshakesWithoutReset)
{
    DiagnosticsFixture fixture("timeout", RouterParams{});
    fixture.handshake();
    fixture.advance(1999u);
    EXPECT_EQ(fixture.snapshot().link_state, "connected");
    fixture.advance(1u);
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "handshaking") << "silence past the timeout restarts the handshake";
    EXPECT_EQ(diagnostics.resets, 0u) << "a timeout is not an MCU reset";
    EXPECT_EQ(diagnostics.session_token, 1u) << "the token survives until the next READY";
}
TEST_F(DiagnosticsNode, ClosedTransportReportsDisconnected)
{
    DiagnosticsFixture fixture("dead", RouterParams{});
    fixture.handshake();
    EXPECT_EQ(fixture.snapshot().session_token, 1u);
    fixture.script_closed();
    fixture.poll_once();
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "disconnected");
    EXPECT_EQ(diagnostics.session_token, 1u) << "the token survives the drop";
    EXPECT_EQ(diagnostics.resets, 0u) << "a dead port is not an MCU reset";
}

TEST_F(DiagnosticsNode, StumbledHandshakeStillConnects)
{
    DiagnosticsFixture fixture("stumble", RouterParams{});
    fixture.deliver_frame(kHelperControl, 0u, helper_hello(7u, 2u));
    EXPECT_EQ(fixture.snapshot().link_state, "handshaking");
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    fixture.deliver_frame(
        kHelperControl, 1u,
        helper_topic(5u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
    EXPECT_EQ(fixture.snapshot().link_state, "handshaking") << "a refused TOPIC restarts the wait";
    fixture.deliver_frame(
        kHelperControl, 2u,
        helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
    const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
    fixture.deliver_frame(
        kHelperControl, 3u,
        helper_topic(1u, kCmdVel, 0x01u, twist_hash.data(), "geometry_msgs/msg/Twist", "cmd_vel"));
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "connected");
    EXPECT_EQ(diagnostics.session_token, 1u);
    EXPECT_EQ(diagnostics.resets, 0u);
}

TEST_F(DiagnosticsNode, TruncatedHelloKeepsBootWait)
{
    DiagnosticsFixture fixture("shorthello", RouterParams{});
    const std::vector<std::uint8_t> full = helper_hello(7u, 2u);
    for (const std::size_t kept : {1u, 3u, 9u}) {
        ASSERT_LT(kept, full.size());
        fixture.deliver_frame(kHelperControl, 0u,
                              std::vector<std::uint8_t>(full.begin(), full.begin() + kept));
        EXPECT_EQ(fixture.snapshot().link_state, "boot_wait") << "a short HELLO is refused";
    }
    EXPECT_EQ(fixture.snapshot().session_token, 0u);
    fixture.deliver_frame(kHelperControl, 1u, full);
    EXPECT_EQ(fixture.snapshot().link_state, "handshaking") << "the wait survives refusals";
}

TEST_F(DiagnosticsNode, MalformedTopicsDoNotStallTheWalk)
{
    DiagnosticsFixture fixture("badtopic", RouterParams{});
    fixture.deliver_frame(kHelperControl, 0u, helper_hello(7u, 2u));
    EXPECT_EQ(fixture.snapshot().link_state, "handshaking");
    const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
    const std::vector<std::uint8_t> head{0x04u, 0u, kBattery, 0x00u};
    std::vector<std::uint8_t> short_hash = head;
    short_hash.insert(short_hash.end(), float_hash.begin(), float_hash.begin() + 3);
    std::vector<std::uint8_t> bad_varint = head;
    bad_varint.insert(bad_varint.end(), float_hash.begin(), float_hash.end());
    bad_varint.push_back(0x80u);
    std::vector<std::uint8_t> trailing =
        helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery");
    trailing.push_back(0x00u);
    const std::vector<std::vector<std::uint8_t>> malformed{
        {{0x04u}},
        {{0x04u, 0u}},
        head,
        short_hash,
        helper_topic(0u, 5u, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"),
        bad_varint,
        trailing,
    };
    for (const std::vector<std::uint8_t> &payload : malformed) {
        fixture.deliver_frame(kHelperControl, 2u, payload);
        EXPECT_EQ(fixture.snapshot().link_state, "handshaking") << "a refused TOPIC restarts the wait";
    }
    fixture.deliver_frame(
        kHelperControl, 3u,
        helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
    const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
    fixture.deliver_frame(
        kHelperControl, 4u,
        helper_topic(1u, kCmdVel, 0x01u, twist_hash.data(), "geometry_msgs/msg/Twist", "cmd_vel"));
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "connected");
    EXPECT_EQ(diagnostics.session_token, 1u);
}

TEST_F(DiagnosticsNode, RepeatedHelloDuringWalkChangesNothing)
{
    DiagnosticsFixture fixture("repeat", RouterParams{});
    const std::vector<std::uint8_t> hello = helper_hello(7u, 2u);
    fixture.deliver_frame(kHelperControl, 0u, hello);
    EXPECT_EQ(fixture.snapshot().link_state, "handshaking");
    fixture.deliver_frame(kHelperControl, 1u, hello);
    EXPECT_EQ(fixture.snapshot().link_state, "handshaking") << "the repeat answers nothing anew";
    EXPECT_EQ(fixture.snapshot().resets, 0u);
    fixture.handshake();
    EXPECT_EQ(fixture.snapshot().session_token, 1u);
}

TEST_F(DiagnosticsNode, RefusedTopicCarriesNoTraffic)
{
    DiagnosticsFixture fixture("nort", RouterParams{});
    fixture.deliver_frame(kHelperControl, 0u, helper_hello(7u, 2u));
    const std::array<std::uint8_t, 8> zero_hash{};
    fixture.deliver_frame(kHelperControl, 1u,
                          helper_topic(0u, kBattery, 0x00u, zero_hash.data(), "nope/Missing", "battery"));
    const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
    fixture.deliver_frame(
        kHelperControl, 2u,
        helper_topic(1u, kCmdVel, 0x01u, twist_hash.data(), "geometry_msgs/msg/Twist", "cmd_vel"));
    EXPECT_EQ(fixture.router().publisher_count(), 0u) << "a refused topic gets no route";
    EXPECT_EQ(fixture.router().subscription_count(), 1u);
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "battery", rclcpp::QoS(10).best_effort(),
        [&received](const std_msgs::msg::Float32 &msg) { received.push_back(msg.data); });
    // No match wait: the refused topic has no publisher, so none can appear.
    // The structural fact is the missing route above; these spins only flush
    // anything already in flight.
    const std::vector<std::uint8_t> one{0x00, 0x00, 0x80, 0x3F};
    fixture.deliver_frame(kBattery, 3u, one);
    rclcpp::spin_some(fixture.node());
    rclcpp::spin_some(fixture.node());
    EXPECT_TRUE(received.empty()) << "frames on a refused topic go nowhere";
}

TEST_F(DiagnosticsNode, FrameBeforeHelloLeavesNoTrace)
{
    DiagnosticsFixture fixture("early", RouterParams{});
    const std::vector<std::uint8_t> one{0x00, 0x00, 0x80, 0x3F};
    fixture.deliver_frame(kBattery, 3u, one);
    EXPECT_EQ(fixture.snapshot().link_state, "boot_wait");
    fixture.handshake();
    fixture.deliver_frame(kBattery, 0u, one);
    const TopicDiagnostics *battery = find_topic(fixture.snapshot(), kBattery);
    ASSERT_NE(battery, nullptr);
    EXPECT_EQ(battery->seq_gaps, 0u) << "traffic before the handshake sets no expectation";
}

TEST_F(DiagnosticsNode, ShortHeartbeatIsIgnored)
{
    DiagnosticsFixture fixture("shorthb", RouterParams{});
    fixture.handshake();
    fixture.deliver_frame(kHeartbeat, 7u, {0x00u, 0x01u});
    const McuCounters counters = fixture.snapshot().mcu_counters;
    EXPECT_EQ(counters.rx_frames, 0u);
    EXPECT_EQ(counters.crc_errors, 0u);
    EXPECT_EQ(counters.rx_overflows, 0u);
    EXPECT_EQ(counters.dropped_frames, 0u);
    EXPECT_EQ(fixture.snapshot().link_state, "connected");
}

TEST_F(DiagnosticsNode, ThrowingFactoryStaysDisconnected)
{
    DiagnosticsFixture fixture("noport", RouterParams{},
                               []() -> std::unique_ptr<Transport> { throw std::runtime_error("no port"); });
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "disconnected") << "a throwing open counts as no port";
    EXPECT_EQ(diagnostics.session_token, 0u);
}

TEST_F(DiagnosticsNode, SilentWalkRetriesTopicReq)
{
    DiagnosticsFixture fixture("retry", RouterParams{});
    fixture.deliver_frame(kHelperControl, 0u, helper_hello(7u, 2u));
    for (int i = 0; i < 4; ++i) {
        fixture.advance(201u);
    }
    const Diagnostics diagnostics = fixture.snapshot();
    EXPECT_EQ(diagnostics.link_state, "handshaking") << "an unanswered walk starts over";
    EXPECT_EQ(diagnostics.resets, 0u);
    EXPECT_EQ(diagnostics.session_token, 7u) << "the MCU echo is kept until READY assigns one";
}

TEST_F(DiagnosticsNode, ReportsKeepLatestReplacementAsDrop)
{
    RouterParams params;
    DiagnosticsFixture fixture("latest", params);
    fixture.handshake_full(7u, 0x05u);
    auto pub = fixture.node()->create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
    fixture.wait_until([&pub]() { return pub->get_subscription_count() > 0u; });
    geometry_msgs::msg::Twist first;
    first.linear.x = 1.0;
    pub->publish(first);
    rclcpp::spin_some(fixture.node());
    geometry_msgs::msg::Twist second;
    second.linear.x = 2.0;
    pub->publish(second);
    fixture.wait_until([&fixture]() { return fixture.router().dropped(kCmdVel) >= 1u; });
    const Diagnostics diagnostics = fixture.snapshot();
    const TopicDiagnostics *cmd_vel = find_topic(diagnostics, kCmdVel);
    ASSERT_NE(cmd_vel, nullptr);
    EXPECT_EQ(cmd_vel->dropped, 1u) << "the replaced message is counted";
}

TEST_F(DiagnosticsNode, ReportsQueueOverflowAsDrop)
{
    DiagnosticsFixture fixture("queue", RouterParams{});
    fixture.handshake(7u);
    auto pub =
        fixture.node()->create_publisher<geometry_msgs::msg::Twist>("cmd_vel", rclcpp::QoS(10).best_effort());
    fixture.wait_until([&pub]() { return pub->get_subscription_count() > 0u; });
    // No poll runs between the batches, so the scheduler queues every batch;
    // the bound is the default depth of 10. Each batch waits for its own
    // arrival first, so a slow match fails loudly instead of skewing the
    // count.
    int submitted = 0;
    for (int batch = 0; batch < 3; ++batch) {
        for (int i = 0; i < 5; ++i) {
            geometry_msgs::msg::Twist twist;
            pub->publish(twist);
            ++submitted;
        }
        fixture.wait_until([&fixture, submitted]() {
            return fixture.router().queued(kCmdVel) + fixture.router().dropped(kCmdVel) ==
                   static_cast<std::size_t>(submitted);
        });
    }
    const Diagnostics diagnostics = fixture.snapshot();
    const TopicDiagnostics *cmd_vel = find_topic(diagnostics, kCmdVel);
    ASSERT_NE(cmd_vel, nullptr);
    EXPECT_EQ(cmd_vel->dropped, 5u) << "a queued topic past its bound drops the oldest";
}

TEST_F(DiagnosticsNode, ReportsTimeSyncAsUnavailable)
{
    DiagnosticsFixture fixture("nosync", RouterParams{});
    fixture.handshake();
    EXPECT_FALSE(fixture.snapshot().time_sync_available) << "the estimator lands with P3";
}

} // namespace
} // namespace test
} // namespace r2s_bridge
