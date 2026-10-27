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

    std::size_t publisher_count() const;
    std::size_t subscription_count() const;
    // The negotiated QoS of one routed topic, for tests and the H8
    // diagnostics. Empty when the topic is not routed.
    std::optional<rclcpp::QoS> endpoint_qos(std::uint8_t topic_id) const;
    // Pacer drops for one MCU topic id, for the H8 diagnostics.
    std::size_t dropped(std::uint8_t topic_id) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__ROUTER_HPP_
