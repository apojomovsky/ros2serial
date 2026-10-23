#include "r2s_bridge/bridge.hpp"

#include <rclcpp_components/register_node_macro.hpp>

#include <string>
#include <vector>

namespace r2s_bridge
{

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
}

} // namespace r2s_bridge

RCLCPP_COMPONENTS_REGISTER_NODE(r2s_bridge::Bridge)
