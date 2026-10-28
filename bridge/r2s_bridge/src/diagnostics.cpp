// Snapshot in, DiagnosticArray out: the H8 values as key/value pairs.
#include "r2s_bridge/diagnostics.hpp"

#include <string>

namespace r2s_bridge
{
namespace
{

void add_value(diagnostic_msgs::msg::DiagnosticStatus &status, const std::string &key,
               const std::string &value)
{
    diagnostic_msgs::msg::KeyValue entry;
    entry.key = key;
    entry.value = value;
    status.values.push_back(entry);
}

void add_topic(diagnostic_msgs::msg::DiagnosticStatus &status, const Router::TopicDiagnostics &topic)
{
    const std::string head = "topic " + topic.topic_name;
    add_value(status, head + " direction", topic.device_to_host ? "device to host" : "host to device");
    const char *verdict = "ok";
    if (topic.status == 1u) {
        verdict = "unknown type";
    } else if (topic.status == 2u) {
        verdict = "hash mismatch";
    } else if (topic.status == 3u) {
        verdict = "name conflict";
    }
    add_value(status, head + " status", verdict);
    add_value(status, head + " seq gaps", std::to_string(topic.seq_gaps));
    add_value(status, head + " dropped", std::to_string(topic.dropped));
}
} // namespace

diagnostic_msgs::msg::DiagnosticArray diagnostics_message(const Router::Diagnostics &snapshot,
                                                          const std::string &node_name,
                                                          const std::string &hardware_id)
{
    diagnostic_msgs::msg::DiagnosticArray out;
    diagnostic_msgs::msg::DiagnosticStatus status;
    status.name = node_name + ": " + kDiagnosticsTaskName;
    status.hardware_id = hardware_id;
    // A silent link is an error; a link mid-handshake is transient.
    if (snapshot.link_state == "connected") {
        status.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
    } else if (snapshot.link_state == "disconnected") {
        status.level = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
    } else {
        status.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
    }
    status.message = snapshot.link_state;
    add_value(status, "session token", std::to_string(snapshot.session_token));
    add_value(status, "resets", std::to_string(snapshot.resets));
    add_value(status, "control coalesced", std::to_string(snapshot.control_coalesced));
    add_value(status, "protocol version", std::to_string(snapshot.protocol_version));
    add_value(status, "mcu rx frames", std::to_string(snapshot.mcu_counters.rx_frames));
    add_value(status, "mcu crc errors", std::to_string(snapshot.mcu_counters.crc_errors));
    add_value(status, "mcu rx overflows", std::to_string(snapshot.mcu_counters.rx_overflows));
    add_value(status, "mcu dropped frames", std::to_string(snapshot.mcu_counters.dropped_frames));
    for (const Router::TopicDiagnostics &topic : snapshot.topics) {
        add_topic(status, topic);
    }
    // The P3 estimator fills these in; until the first CORRECTION the keys
    // stay put with no value, so a consumer can rely on their names.
    add_value(status, "time sync offset",
              snapshot.time_sync_available ? std::to_string(snapshot.time_offset_ns) : "not available");
    add_value(status, "time sync drift",
              snapshot.time_sync_available ? std::to_string(snapshot.time_drift_ppm) : "not available");
    add_value(status, "time sync round trip",
              snapshot.time_sync_available ? std::to_string(snapshot.time_round_trip_ns) : "not available");
    out.status.push_back(status);
    return out;
}
} // namespace r2s_bridge
