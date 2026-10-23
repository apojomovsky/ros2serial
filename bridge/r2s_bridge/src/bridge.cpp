#include "r2s_bridge/bridge.hpp"

#include <rclcpp_components/register_node_macro.hpp>

namespace r2s_bridge
{

Bridge::Bridge(const rclcpp::NodeOptions &options) : rclcpp::Node("r2s_bridge", options)
{
}

} // namespace r2s_bridge

RCLCPP_COMPONENTS_REGISTER_NODE(r2s_bridge::Bridge)
