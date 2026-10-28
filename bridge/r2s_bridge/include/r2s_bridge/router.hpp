// The link between the MCU's topics and the ROS graph: generic publishers
// and subscriptions under the announced names, paced host-to-MCU traffic,
// and converted payloads both ways. A thin shell over Session, Converter
// and Scheduler; the node only owns it and polls it.
#ifndef R2S_BRIDGE__ROUTER_HPP_
#define R2S_BRIDGE__ROUTER_HPP_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "r2s_bridge/scheduler.hpp"
#include "r2s_bridge/session.hpp"

namespace r2s_bridge
{

class Converter;

// Where the router logs: tests capture the sink, production forwards it to
// the node logger with its severity.
using LogSink = std::function<void(SessionLog, const std::string &)>;

// The bridge parameters behind the pacing (docs/00-design.md, E4).
struct RouterParams {
    double rate_default_hz = 0.0;
    std::map<std::string, double> rate_limits;
    int tx_budget_pct = 50;
    std::size_t qos_depth = 10;
    int baud = 115200;
};

// Parses "<topic>=<hz>" entries. A malformed entry is reported through the
// sink and skipped, and a negative rate is malformed too.
std::map<std::string, double> parse_rate_overrides(const std::vector<std::string> &entries,
                                                   const LogSink &sink);

// Routes one link. Single-threaded apart from the subscription callbacks, so
// one mutex guards the pacer and the routes; the converter is thread-safe.
class Router
{
  public:
    // Throws std::invalid_argument on a nonsense param (no QoS depth, a
    // budget outside 0 to 100, no baud, a negative default rate) or a missing
    // log sink.
    // session_config carries the H4 link timing; the pacing params above are
    // the H7 ones.
    Router(rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics, TransportFactory open,
           const Clock &clock, RouterParams params, LogSink sink,
           SessionConfig session_config = SessionConfig());
    ~Router();
    Router(const Router &) = delete;

    // Advances the session and flushes due host-to-MCU frames.
    void poll();

    // One routed topic's H8 values: the announced name and direction, the
    // host's TOPIC_STATUS verdict for it ([CTL-13]), the MCU-to-host frames
    // it skipped ([FRM-15]), and the host-to-MCU payloads the pacer dropped
    // ([SES-26], [SES-30]). The direction the topic never uses reads 0.
    // Every announced topic is listed, including one the host refused: its
    // verdict says why it carries no traffic.
    struct TopicDiagnostics {
        std::uint8_t topic_id = 0;
        std::string topic_name;
        bool device_to_host = true;
        std::uint8_t status = 0;
        std::uint32_t seq_gaps = 0;
        std::size_t dropped = 0;
    };
    // The H8 snapshot: link state is one of disconnected, boot_wait,
    // handshaking or connected; time_sync_available stays false until the P3
    // estimator lands, and the bridge reports it as not available.
    struct Diagnostics {
        std::string link_state;
        std::uint32_t session_token = 0;
        std::uint32_t resets = 0;
        // Control frames a newer frame of the same slot replaced while the link
        // could not take bytes ([SES-33]).
        std::uint32_t control_coalesced = 0;
        std::uint8_t protocol_version = 0;
        McuCounters mcu_counters;
        std::vector<TopicDiagnostics> topics;
        bool time_sync_available = false;
    };

    std::size_t publisher_count() const;
    std::size_t subscription_count() const;
    // The negotiated QoS of one routed topic, for tests and the H8
    // diagnostics. Empty when the topic is not routed.
    std::optional<rclcpp::QoS> endpoint_qos(std::uint8_t topic_id) const;
    // Pacer drops for one MCU topic id, for the H8 diagnostics.
    std::size_t dropped(std::uint8_t topic_id) const;
    // Queued frames for one MCU topic id, so the H8 test can wait for every
    // scripted message to arrive before it checks the drops.
    std::size_t queued(std::uint8_t topic_id) const;
    // The H8 values after scripted losses and errors, for /diagnostics.
    Diagnostics diagnostics() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__ROUTER_HPP_
