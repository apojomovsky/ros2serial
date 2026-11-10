// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The link between the MCU's topics and the ROS graph: generic publishers
// and subscriptions under the announced names, paced host-to-MCU traffic,
// and converted payloads both ways. A thin shell over Session, Converter
// and Scheduler; the node only owns it and polls it.
#ifndef R2S_BRIDGE__ROUTER_HPP_
#define R2S_BRIDGE__ROUTER_HPP_

#include <array>
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

/// Converts packed wire payloads to serialized CDR.
class Converter;

/// Where the router logs: tests capture the sink.
/**
 * Production forwards it to the node logger with its severity.
 */
using LogSink = std::function<void(SessionLog, const std::string &)>;

/// The node interfaces the service proxies need beyond topics.
/**
 * Base for the rcl handle behind the service server, graph for generic
 * clients, services to register a generic service where the distro has
 * one. All null skips the service proxies with a precise log, while
 * topics keep routing.
 */
struct NodeServiceInterfaces
{
  rclcpp::node_interfaces::NodeBaseInterface::SharedPtr base;
  rclcpp::node_interfaces::NodeGraphInterface::SharedPtr graph;
  rclcpp::node_interfaces::NodeServicesInterface::SharedPtr services;
};

/// The bridge parameters behind the pacing (docs/00-design.md, E4).
struct RouterParams
{
  double rate_default_hz = 0.0;  ///< The default send rate, 0 for none.
  std::map<std::string, double> rate_limits;  ///< Per-topic send rates.
  int tx_budget_pct = 50;  ///< The link share for host-to-MCU traffic.
  std::size_t qos_depth = 10;  ///< The endpoint queue depth.
  int baud = 115200;  ///< Bits per second.
};

/// Parses "<topic>=<hz>" entries.
/**
 * A malformed entry is reported through the sink and skipped, and a
 * negative rate is malformed too.
 *
 * \param[in] entries the "<topic>=<hz>" lines to parse.
 * \param[in] sink where to report malformed entries.
 * \return The parsed per-topic rates.
 */
std::map<std::string, double> parse_rate_overrides(
  const std::vector<std::string> & entries,
  const LogSink & sink);

/// Routes one link.
/**
 * Single-threaded apart from the subscription callbacks, so one mutex
 * guards the pacer and the routes; the converter is thread-safe.
 */
class Router
{
public:
  /// Starts the routes over one link.
  /**
   * session_config carries the H4 link timing; the pacing params above are
   * the RouterParams. mcu_sink carries the MCU's own log lines ([LOG-1])
   * and defaults to the log sink. node_params carries the interface the
   * parameters layer (X5) declares the MCU's parameters on; null skips it.
   *
   * \param[in] topics the node topics interface for the endpoints.
   * \param[in] open the port factory.
   * \param[in] clock the steady clock for pacing and timers.
   * \param[in] ros_clock the ROS clock for CORRECTION stamps.
   * \param[in] params the pacing parameters.
   * \param[in] sink where the router logs.
   * \param[in] session_config the H4 link timing.
   * \param[in] mcu_sink where the MCU log lines go.
   * \param[in] node_params the parameters interface, or null to skip it.
   * \throws std::invalid_argument on a nonsense param (no QoS depth, a
   * budget outside 0 to 100, no baud, a negative default rate) or a
   * missing log sink.
   */
  Router(
    rclcpp::node_interfaces::NodeTopicsInterface::SharedPtr topics, TransportFactory open,
    const Clock & clock, const Clock & ros_clock, RouterParams params, LogSink sink,
    SessionConfig session_config = SessionConfig(), LogSink mcu_sink = LogSink(),
    rclcpp::node_interfaces::NodeParametersInterface::SharedPtr node_params = nullptr,
    NodeServiceInterfaces node_services = NodeServiceInterfaces());

  /// Destroys the router and its routes.
  ~Router();

  /// Not copyable.
  Router(const Router &) = delete;

  /// Advances the session and flushes due host-to-MCU frames.
  void poll();

  /// One announced topic's H8 values.
  /**
   * The announced name and direction, the host's TOPIC_STATUS verdict for
   * it ([CTL-13]), the MCU-to-host frames it skipped ([FRM-15]), the
   * host-to-MCU payloads the pacer dropped ([SES-26], [SES-30]), and the
   * converted frames a failed serialized publish dropped instead of
   * aborting. The direction the topic never uses reads 0. Every announced
   * topic is listed in announcement order, including one the host
   * refused: its verdict says why it carries no traffic.
   */
  struct TopicDiagnostics
  {
    std::uint8_t topic_id = 0;  ///< The wire id.
    std::string topic_name;  ///< The announced topic name.
    bool device_to_host = true;  ///< True when the MCU publishes the topic.
    std::uint8_t status = 0;  ///< The host verdict [CTL-13].
    std::uint32_t seq_gaps = 0;  ///< Skipped MCU-to-host frames [FRM-15].
    std::size_t dropped = 0;  ///< Pacer-dropped host-to-MCU payloads.
    /// Converted device-to-host frames the serialized publish refused.
    /**
     * Logged and counted instead of aborting.
     */
    std::size_t publish_failures = 0;
    /// MCU error report counts naming this topic id ([ERR-6]).
    /**
     * Slot 0 carries code 1 through slot 5 code 6. Zeros when no report
     * named this id.
     */
    std::array<std::uint64_t, 6> mcu_errors{};
    /// Whether the verdict is ok but no route serves the topic.
    /**
     * The endpoint creation failed and later polls keep retrying it.
     * False for refused topics, which correctly serve nothing, and once
     * a retry heals the route.
     */
    bool route_missing = false;
    /// Why the route is missing, meaningful only when route_missing holds.
    std::string route_error;
  };

  /// The H8 snapshot.
  /**
   * link state is one of disconnected, boot_wait, handshaking or
   * connected; the time sync values are real once a CORRECTION anchors
   * the estimator, and read 0 with available false before that.
   */
  struct Diagnostics
  {
    std::string link_state;  ///< The session state name.
    std::uint32_t session_token = 0;  ///< The session token [SES-19].
    std::uint32_t resets = 0;  ///< HELLOs accepted while connected.
    /// Control frames a newer frame of the same slot replaced.
    /**
     * While the link could not take bytes ([SES-33]).
     */
    std::uint32_t control_coalesced = 0;
    std::uint8_t protocol_version = 0;  ///< The negotiated wire version.
    McuCounters mcu_counters;  ///< The heartbeat counters [HB-2].
    /// Running MCU error report sums per code ([ERR-6]).
    /**
     * Slot 0 carries code 1 through slot 5 code 6, counting targeted
     * and untargeted reports alike. Zeros before the first report.
     */
    std::array<std::uint64_t, 6> mcu_error_sums{};
    std::vector<TopicDiagnostics> topics;  ///< One entry per announced topic.
    bool time_sync_available = false;  ///< Whether a correction is anchored.
    std::int64_t time_offset_ns = 0;  ///< The fitted offset.
    std::int16_t time_drift_ppm = 0;  ///< The fitted drift.
    std::int64_t time_round_trip_ns = 0;  ///< The latest round trip.
  };

  /// How many publishers the router serves.
  /**
   * \return The device-to-host route count.
   */
  std::size_t publisher_count() const;

  /// How many subscriptions the router serves.
  /**
   * \return The host-to-device route count.
   */
  std::size_t subscription_count() const;

  /// The negotiated QoS of one routed topic.
  /**
   * For tests and the H8 diagnostics. Empty when the topic is not routed.
   *
   * \param[in] topic_id whose QoS to report.
   * \return The negotiated QoS, or empty.
   */
  std::optional<rclcpp::QoS> endpoint_qos(std::uint8_t topic_id) const;

  /// Pacer drops for one MCU topic id, for the H8 diagnostics.
  /**
   * \param[in] topic_id whose drops to count.
   * \return How many payloads the pacer dropped.
   */
  std::size_t dropped(std::uint8_t topic_id) const;

  /// Queued frames for one MCU topic id.
  /**
   * The H8 test waits for every scripted message to arrive through this
   * before it checks the drops.
   *
   * \param[in] topic_id whose queue to count.
   * \return How many frames wait.
   */
  std::size_t queued(std::uint8_t topic_id) const;

  /// The H8 values after scripted losses and errors, for /diagnostics.
  /**
   * \return The snapshot.
   */
  Diagnostics diagnostics() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__ROUTER_HPP_
