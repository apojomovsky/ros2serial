// The host bridge node: parameters in, a Router out, polled every millisecond
// so the pacer and the heartbeats track the link latency.
#include "r2s_bridge/bridge.hpp"

#include <rclcpp_components/register_node_macro.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "r2s_bridge/termios_transport.hpp"

namespace r2s_bridge
{
namespace
{

constexpr std::chrono::milliseconds kPollPeriod{1};

} // namespace

Bridge::Bridge(const rclcpp::NodeOptions &options) : rclcpp::Node("r2s_bridge", options)
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

    RouterParams pacing;
    pacing.rate_default_hz = get_parameter("rate_limit.default_hz").as_double();
    pacing.rate_limits = parse_rate_overrides(
        get_parameter("rate_limit.overrides").as_string_array(),
        [this](const std::string &text) { RCLCPP_ERROR(get_logger(), "%s", text.c_str()); });
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

    TermiosOptions serial;
    serial.port = get_parameter("port").as_string();
    serial.baud = pacing.baud;
    serial.low_latency = get_parameter("low_latency").as_bool();
    serial.hupcl = get_parameter("hupcl").as_bool();

    router_ = std::make_unique<Router>(
        get_node_topics_interface(), get_logger(),
        [serial]() { return std::unique_ptr<Transport>(new TermiosTransport(serial)); }, clock_,
        std::move(pacing), nullptr, std::move(link));
    timer_ = create_wall_timer(kPollPeriod, [this]() { router_->poll(); });
}

Bridge::~Bridge() = default;

} // namespace r2s_bridge

RCLCPP_COMPONENTS_REGISTER_NODE(r2s_bridge::Bridge)
