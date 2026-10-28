// The /diagnostics payload rendering: a fabricated snapshot in, a
// DiagnosticArray out, through the real builder and no node.
#include <gtest/gtest.h>

#include <map>
#include <string>

#include <diagnostic_msgs/msg/diagnostic_array.hpp>

#include "r2s_bridge/diagnostics.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

std::map<std::string, std::string> values_of(const diagnostic_msgs::msg::DiagnosticStatus &status)
{
    std::map<std::string, std::string> out;
    for (const diagnostic_msgs::msg::KeyValue &value : status.values) {
        out[value.key] = value.value;
    }
    return out;
}

Router::Diagnostics connected_snapshot()
{
    Router::Diagnostics snapshot;
    snapshot.time_sync_available = true;
    snapshot.time_offset_ns = -1500;
    snapshot.time_drift_ppm = 12;
    snapshot.time_round_trip_ns = 2300000;
    snapshot.link_state = "connected";
    snapshot.session_token = 1u;
    snapshot.resets = 2u;
    snapshot.control_coalesced = 6u;
    snapshot.protocol_version = 2u;
    snapshot.mcu_counters.rx_frames = 10u;
    snapshot.mcu_counters.crc_errors = 2u;
    snapshot.mcu_counters.rx_overflows = 1u;
    snapshot.mcu_counters.dropped_frames = 3u;
    Router::TopicDiagnostics battery;
    battery.topic_id = 16u;
    battery.topic_name = "battery";
    battery.device_to_host = true;
    battery.seq_gaps = 4u;
    battery.dropped = 0u;
    snapshot.topics.push_back(battery);
    Router::TopicDiagnostics cmd_vel;
    cmd_vel.topic_id = 17u;
    cmd_vel.topic_name = "cmd_vel";
    cmd_vel.device_to_host = false;
    cmd_vel.seq_gaps = 0u;
    cmd_vel.dropped = 5u;
    snapshot.topics.push_back(cmd_vel);
    return snapshot;
}

TEST(DiagnosticsMessage, ReportsConnectedSnapshot)
{
    const diagnostic_msgs::msg::DiagnosticArray message =
        diagnostics_message(connected_snapshot(), "test_node", "/dev/ttyACM0");
    ASSERT_EQ(message.status.size(), 1u);
    const diagnostic_msgs::msg::DiagnosticStatus &status = message.status[0];
    EXPECT_EQ(status.name, "test_node: link");
    EXPECT_EQ(status.hardware_id, "/dev/ttyACM0");
    EXPECT_EQ(status.level, diagnostic_msgs::msg::DiagnosticStatus::OK);
    EXPECT_EQ(status.message, "connected");
    const std::map<std::string, std::string> values = values_of(status);
    EXPECT_EQ(values.at("session token"), "1");
    EXPECT_EQ(values.at("resets"), "2");
    EXPECT_EQ(values.at("protocol version"), "2");
    EXPECT_EQ(values.at("mcu rx frames"), "10");
    EXPECT_EQ(values.at("mcu crc errors"), "2");
    EXPECT_EQ(values.at("mcu rx overflows"), "1");
    EXPECT_EQ(values.at("mcu dropped frames"), "3");
    EXPECT_EQ(values.at("topic battery direction"), "device to host");
    EXPECT_EQ(values.at("topic battery seq gaps"), "4");
    EXPECT_EQ(values.at("topic battery dropped"), "0");
    EXPECT_EQ(values.at("topic cmd_vel direction"), "host to device");
    EXPECT_EQ(values.at("topic cmd_vel seq gaps"), "0");
    EXPECT_EQ(values.at("topic cmd_vel dropped"), "5");
    EXPECT_EQ(values.at("control coalesced"), "6");
    EXPECT_EQ(values.at("time sync offset"), "-1500");
    EXPECT_EQ(values.at("time sync drift"), "12");
    EXPECT_EQ(values.at("time sync round trip"), "2300000");
}
TEST(DiagnosticsMessage, ReportsPerTopicStatus)
{
    Router::Diagnostics snapshot;
    snapshot.link_state = "connected";
    const char *names[] = {"ok_topic", "unknown_topic", "mismatch_topic", "dup"};
    const std::uint8_t verdicts[] = {0u, 1u, 2u, 3u};
    for (int i = 0; i < 4; ++i) {
        Router::TopicDiagnostics topic;
        topic.topic_id = static_cast<std::uint8_t>(16u + i);
        topic.topic_name = names[i];
        topic.device_to_host = true;
        topic.status = verdicts[i];
        snapshot.topics.push_back(topic);
    }
    const std::map<std::string, std::string> values =
        values_of(diagnostics_message(snapshot, "test_node", "").status[0]);
    EXPECT_EQ(values.at("topic ok_topic status"), "ok");
    EXPECT_EQ(values.at("topic unknown_topic status"), "unknown type");
    EXPECT_EQ(values.at("topic mismatch_topic status"), "hash mismatch");
    EXPECT_EQ(values.at("topic dup status"), "name conflict");
}

TEST(DiagnosticsMessage, MapsLinkStateToLevel)
{
    Router::Diagnostics down;
    down.link_state = "disconnected";
    const auto named = [](const Router::Diagnostics &snapshot) {
        return diagnostics_message(snapshot, "test_node", "").status[0];
    };
    EXPECT_EQ(named(down).level, diagnostic_msgs::msg::DiagnosticStatus::ERROR);
    EXPECT_EQ(named(down).name, "test_node: link") << "a remapped bridge stays recognizable";
    Router::Diagnostics waiting;
    waiting.link_state = "boot_wait";
    EXPECT_EQ(named(waiting).level, diagnostic_msgs::msg::DiagnosticStatus::WARN);
    Router::Diagnostics shaking;
    shaking.link_state = "handshaking";
    EXPECT_EQ(named(shaking).message, "handshaking");
    EXPECT_EQ(named(shaking).level, diagnostic_msgs::msg::DiagnosticStatus::WARN);
}

} // namespace
} // namespace test
} // namespace r2s_bridge
