// The /diagnostics payload rendering: a fabricated snapshot in, a
// DiagnosticArray out, through the real builder and no node.
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

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
    snapshot.mcu_error_sums = {{0u, 0u, 7u, 0u, 0u, 9u}};
    Router::TopicDiagnostics battery;
    battery.topic_id = 16u;
    battery.topic_name = "battery";
    battery.device_to_host = true;
    battery.seq_gaps = 4u;
    battery.dropped = 0u;
    battery.publish_failures = 1u;
    battery.mcu_errors = {{0u, 2u, 0u, 0u, 0u, 5u}};
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
    EXPECT_EQ(values.at("mcu error code 1"), "0");
    EXPECT_EQ(values.at("mcu error code 2"), "0");
    EXPECT_EQ(values.at("mcu error code 3"), "7");
    EXPECT_EQ(values.at("mcu error code 4"), "0");
    EXPECT_EQ(values.at("mcu error code 5"), "0");
    EXPECT_EQ(values.at("mcu error code 6"), "9");
    EXPECT_EQ(values.at("topic 16 battery mcu errors"), "1=0 2=2 3=0 4=0 5=0 6=5");
    EXPECT_EQ(values.at("topic 17 cmd_vel mcu errors"), "1=0 2=0 3=0 4=0 5=0 6=0");
    EXPECT_EQ(values.at("topic 16 battery direction"), "device to host");
    EXPECT_EQ(values.at("topic 16 battery seq gaps"), "4");
    EXPECT_EQ(values.at("topic 16 battery dropped"), "0");
    EXPECT_EQ(values.at("topic 16 battery publish failures"), "1");
    EXPECT_EQ(values.at("topic 17 cmd_vel direction"), "host to device");
    EXPECT_EQ(values.at("topic 17 cmd_vel seq gaps"), "0");
    EXPECT_EQ(values.at("topic 17 cmd_vel dropped"), "5");
    EXPECT_EQ(values.at("mcu error code 1"), "0");
    EXPECT_EQ(values.at("mcu error code 2"), "0");
    EXPECT_EQ(values.at("mcu error code 3"), "7");
    EXPECT_EQ(values.at("mcu error code 4"), "0");
    EXPECT_EQ(values.at("mcu error code 5"), "0");
    EXPECT_EQ(values.at("mcu error code 6"), "9");
    EXPECT_EQ(values.at("topic 16 battery mcu errors"), "1=0 2=2 3=0 4=0 5=0 6=5");
    EXPECT_EQ(values.at("topic 17 cmd_vel mcu errors"), "1=0 2=0 3=0 4=0 5=0 6=0");
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
    EXPECT_EQ(values.at("topic 16 ok_topic status"), "ok");
    EXPECT_EQ(values.at("topic 17 unknown_topic status"), "unknown type");
    EXPECT_EQ(values.at("topic 18 mismatch_topic status"), "hash mismatch");
    EXPECT_EQ(values.at("topic 19 dup status"), "name conflict");
}

TEST(DiagnosticsMessage, SameNameTopicsKeepDistinctKeys)
{
    Router::Diagnostics snapshot;
    snapshot.link_state = "connected";
    Router::TopicDiagnostics first;
    first.topic_id = 16u;
    first.topic_name = "dup";
    first.device_to_host = true;
    first.status = 0u;
    first.seq_gaps = 4u;
    snapshot.topics.push_back(first);
    Router::TopicDiagnostics second;
    second.topic_id = 17u;
    second.topic_name = "dup";
    second.device_to_host = false;
    second.status = 3u;
    second.dropped = 5u;
    snapshot.topics.push_back(second);
    const diagnostic_msgs::msg::DiagnosticArray message = diagnostics_message(snapshot, "test_node", "");
    ASSERT_EQ(message.status.size(), 1u);
    std::map<std::string, std::string> values;
    for (const diagnostic_msgs::msg::KeyValue &value : message.status[0].values) {
        ASSERT_TRUE(values.emplace(value.key, value.value).second) << "duplicate key " << value.key;
    }
    EXPECT_EQ(values.at("topic 16 dup direction"), "device to host");
    EXPECT_EQ(values.at("topic 16 dup status"), "ok");
    EXPECT_EQ(values.at("topic 16 dup seq gaps"), "4");
    EXPECT_EQ(values.at("topic 17 dup direction"), "host to device");
    EXPECT_EQ(values.at("topic 17 dup status"), "name conflict");
    EXPECT_EQ(values.at("topic 17 dup dropped"), "5");
}

// Two announced topics sharing one name still render distinct error keys:
// the topic id heads every per-topic error key, as #271's scheme requires.
TEST(DiagnosticsMessage, DisambiguatesSameNameErrorKeysByTopicId)
{
    Router::Diagnostics snapshot;
    snapshot.link_state = "connected";
    for (const std::uint8_t id : {16u, 17u}) {
        Router::TopicDiagnostics topic;
        topic.topic_id = id;
        topic.topic_name = "dup";
        topic.device_to_host = true;
        topic.mcu_errors = {{0u, 0u, 0u, 0u, 0u, static_cast<std::uint64_t>(id)}};
        snapshot.topics.push_back(topic);
    }
    const diagnostic_msgs::msg::DiagnosticArray message = diagnostics_message(snapshot, "test_node", "");
    const diagnostic_msgs::msg::DiagnosticStatus &status = message.status[0];
    std::vector<std::string> keys;
    for (const diagnostic_msgs::msg::KeyValue &value : status.values) {
        keys.push_back(value.key);
    }
    EXPECT_EQ(std::count(keys.begin(), keys.end(), "topic 16 dup mcu errors"), 1);
    EXPECT_EQ(std::count(keys.begin(), keys.end(), "topic 17 dup mcu errors"), 1);
    const std::map<std::string, std::string> values = values_of(status);
    EXPECT_EQ(values.at("topic 16 dup mcu errors"), "1=0 2=0 3=0 4=0 5=0 6=16");
    EXPECT_EQ(values.at("topic 17 dup mcu errors"), "1=0 2=0 3=0 4=0 5=0 6=17");
}

TEST(DiagnosticsMessage, RendersHugeTimeSyncValues)
{
    Router::Diagnostics snapshot = connected_snapshot();
    snapshot.time_offset_ns = -9223372036854775807LL;
    snapshot.time_drift_ppm = -12345;
    snapshot.time_round_trip_ns = 9223372036854775807LL;
    const std::map<std::string, std::string> values =
        values_of(diagnostics_message(snapshot, "test_node", "").status[0]);
    EXPECT_EQ(values.at("time sync offset"), "-9223372036854775807");
    EXPECT_EQ(values.at("time sync drift"), "-12345");
    EXPECT_EQ(values.at("time sync round trip"), "9223372036854775807");
}

TEST(DiagnosticsMessage, NamesTimeSyncKeysBeforeTheFirstCorrection)
{
    Router::Diagnostics snapshot;
    snapshot.link_state = "connected";
    snapshot.time_sync_available = false;
    const std::map<std::string, std::string> values =
        values_of(diagnostics_message(snapshot, "test_node", "").status[0]);
    EXPECT_EQ(values.at("time sync offset"), "not available");
    EXPECT_EQ(values.at("time sync drift"), "not available");
    EXPECT_EQ(values.at("time sync round trip"), "not available");
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
