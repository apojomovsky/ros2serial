// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The host bridge node: parameters in, a Router out, woken by the port's
// readiness for the receive path and by ROS arrivals for the transmit path,
// and polled on a timer for everything else.
#include "r2s_bridge/bridge.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <rclcpp_components/register_node_macro.hpp>

#include "r2s_bridge/diagnostics.hpp"
#include "r2s_bridge/termios_transport.hpp"

namespace r2s_bridge
{
namespace
{

// The receive and transmit paths no longer wait for this period: a byte
// wakes the executor through the link waitable and a ROS arrival through
// the kick waitable, so the timer only paces, beats, reconnects and
// reports, and backs up both halves. Each of those is due on a deadline of
// its own, so
// a period far under the shortest of them (the 500 ms heartbeat) keeps them
// accurate while the node sleeps instead of running a pass 1000 times a
// second.
constexpr std::chrono::milliseconds kPollPeriod{20};
// Diagnostics ride the poll timer instead of their own: the snapshot must
// read on the thread that polls the router, whose session keeps no lock.
// The deadline is wall-clock, so the period does not drift when polling slows.
constexpr std::chrono::seconds kDiagnosticsPeriod{1};

}  // namespace
Bridge::Bridge(const rclcpp::NodeOptions & options)
: rclcpp::Node("r2s_bridge", options), ros_clock_([this]() {return now().nanoseconds();})
{
  declare_parameter<std::string>("port", "/dev/ttyACM0");
  declare_parameter<int>("baud", 115200);
  declare_parameter<bool>("low_latency", true);
  declare_parameter<bool>("hupcl", true);
  declare_parameter<int>("boot_wait_ms", 2500);
  declare_parameter<std::string>("node_map", "");
  declare_parameter<int>("reconnect_period_ms", 1000);
  declare_parameter<int>("heartbeat_period_ms", 500);
  declare_parameter<int>("link_timeout_ms", 2000);
  declare_parameter<bool>("time_sync.enabled", true);
  declare_parameter<int>("time_sync.period_ms", 5000);
  declare_parameter<double>("rate_limit.default_hz", 0.0);
  declare_parameter<std::vector<std::string>>("rate_limit.overrides", std::vector<std::string>{});
  declare_parameter<int>("tx_budget_pct", 50);
  declare_parameter<int>("qos_depth", 10);

    // The bridge's own lines stay on the node logger; the MCU's lines go to
    // the mcu child, so they reach /rosout under <node>.mcu (docs/00-design.md,
    // E4) with the severity the log frame names.
  const rclcpp::Logger mcu_log = get_logger().get_child("mcu");
  const auto forward_mcu_log = [mcu_log](SessionLog level, const std::string & text) {
      switch (level) {
        case SessionLog::kDebug:
          RCLCPP_DEBUG(mcu_log, "%s", text.c_str());
          break;
        case SessionLog::kInfo:
          RCLCPP_INFO(mcu_log, "%s", text.c_str());
          break;
        case SessionLog::kWarning:
          RCLCPP_WARN(mcu_log, "%s", text.c_str());
          break;
        case SessionLog::kError:
          RCLCPP_ERROR(mcu_log, "%s", text.c_str());
          break;
        case SessionLog::kFatal:
          RCLCPP_FATAL(mcu_log, "%s", text.c_str());
          break;
      }
    };
  const auto forward_log = [this](SessionLog level, const std::string & text) {
      switch (level) {
        case SessionLog::kDebug:
          RCLCPP_DEBUG(get_logger(), "%s", text.c_str());
          break;
        case SessionLog::kError:
          RCLCPP_ERROR(get_logger(), "%s", text.c_str());
          break;
        case SessionLog::kWarning:
          RCLCPP_WARN(get_logger(), "%s", text.c_str());
          break;
        case SessionLog::kInfo:
          RCLCPP_INFO(get_logger(), "%s", text.c_str());
          break;
        case SessionLog::kFatal:
          RCLCPP_FATAL(get_logger(), "%s", text.c_str());
          break;
      }
    };
  RouterParams pacing;
  pacing.rate_default_hz = get_parameter("rate_limit.default_hz").as_double();
  pacing.rate_limits =
    parse_rate_overrides(get_parameter("rate_limit.overrides").as_string_array(), forward_log);
  pacing.tx_budget_pct = static_cast<int>(get_parameter("tx_budget_pct").as_int());
  const int depth = static_cast<int>(get_parameter("qos_depth").as_int());
  if (depth < 1) {
    throw std::invalid_argument("r2s_bridge: qos_depth must be at least 1");
  }
  pacing.qos_depth = static_cast<std::size_t>(depth);
  pacing.baud = static_cast<int>(get_parameter("baud").as_int());

  SessionConfig link;
  link.reconnect_period_ms = static_cast<int>(get_parameter("reconnect_period_ms").as_int());
  link.boot_wait_ms = static_cast<int>(get_parameter("boot_wait_ms").as_int());
  link.heartbeat_period_ms = static_cast<int>(get_parameter("heartbeat_period_ms").as_int());
  link.link_timeout_ms = static_cast<int>(get_parameter("link_timeout_ms").as_int());
    // The session refuses a period outside 1 to 5000 ms, because SES-25
    // needs a correction at least every 5 s.
  link.time_sync_enabled = get_parameter("time_sync.enabled").as_bool();
    // Published from the session's own thread on every open and close; the
    // waitable is made below, so the first open lands before it exists and
    // the watch is installed from the descriptor it left behind.
  link.on_port = [this](int fd) {on_port(fd);};
  link.time_sync_period_ms = static_cast<int>(get_parameter("time_sync.period_ms").as_int());
  TermiosOptions serial;
  serial.port = get_parameter("port").as_string();
  serial.baud = pacing.baud;
  serial.low_latency = get_parameter("low_latency").as_bool();
  serial.hupcl = get_parameter("hupcl").as_bool();

  NodeServiceInterfaces node_services;
  node_services.base = get_node_base_interface();
  node_services.graph = get_node_graph_interface();
  node_services.services = get_node_services_interface();
  router_ = std::make_unique<Router>(
        get_node_topics_interface(),
    [serial]() {
      return std::unique_ptr<Transport>(new TermiosTransport(serial));
      }, clock_, ros_clock_,
        std::move(pacing), forward_log, std::move(link), forward_mcu_log,
      get_node_parameters_interface(), std::move(node_services));
    // The transmit path is woken by ROS arrivals, not by the timer: the
    // kick watches no descriptor and runs the transmit half on this same
    // thread. The router is told before any route exists, so no callback
    // can fire unkicked.
  kick_ = std::make_shared<LinkWaitable>(
    get_node_base_interface()->get_context(), [this]() {on_ros_work();});
  kick_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  get_node_waitables_interface()->add_waitable(kick_, kick_group_);
  kick_->watch(-1);
  router_->set_kick([this] {kick_->trigger();});
  diagnostics_hardware_id_ = serial.port;
  diagnostics_pub_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>("/diagnostics", 10);
    // The receive path is woken by the port, not by the timer: the waitable
    // watches the descriptor the session publishes on every open, and runs
    // the receive half on this same thread. A link without a descriptor
    // leaves it inert and the timer as the only reader, as before.
  link_ = std::make_shared<LinkWaitable>(
    get_node_base_interface()->get_context(), [this]() {on_readable();});
    // Its own mutually exclusive group, which a single-threaded executor
    // serialises with everything else anyway: the group exists because the
    // waitables interface requires one, not to run anything beside the timer.
  link_group_ = create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  get_node_waitables_interface()->add_waitable(link_, link_group_);
  link_->watch(link_fd_);
  timer_ = create_wall_timer(kPollPeriod, [this]() {poll();});
}

void Bridge::on_port(int fd)
{
  link_fd_ = fd;
  if (link_) {
    link_->watch(fd);
  }
}

void Bridge::on_readable()
{
  router_->receive();
}

void Bridge::on_ros_work()
{
  router_->transmit();
}

const std::shared_ptr<LinkWaitable> & Bridge::link_waitable() const
{
  return link_;
}

void Bridge::poll()
{
  router_->poll();
  const std::chrono::nanoseconds tick = clock_.now();
  if (tick - last_diagnostics_ >= kDiagnosticsPeriod) {
    last_diagnostics_ = tick;
    diagnostic_msgs::msg::DiagnosticArray message =
      diagnostics_message(router_->diagnostics(), get_name(), diagnostics_hardware_id_);
    message.header.stamp = now();
    try {
      publish_diagnostics(message);
    } catch (const std::exception & error) {
            // A failed /diagnostics publish (a context shutting down under
            // the poll loop) is a dropped message, never an abort.
      RCLCPP_WARN(get_logger(), "topic '/diagnostics': failed to publish (%s)", error.what());
    }
  }
}

void Bridge::publish_diagnostics(const diagnostic_msgs::msg::DiagnosticArray & message)
{
  diagnostics_pub_->publish(message);
}

Bridge::~Bridge() = default;

}  // namespace r2s_bridge

RCLCPP_COMPONENTS_REGISTER_NODE(r2s_bridge::Bridge)
