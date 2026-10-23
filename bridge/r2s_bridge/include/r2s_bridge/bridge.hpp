// The host bridge node, one instance per serial port. Parameters are declared
// on construction so `ros2 param` can list and set them before a link exists;
// the transport and session that consume them are not part of this skeleton.
#ifndef R2S_BRIDGE__BRIDGE_HPP_
#define R2S_BRIDGE__BRIDGE_HPP_

#include <rclcpp/rclcpp.hpp>

namespace r2s_bridge
{

class Bridge : public rclcpp::Node
{
  public:
    explicit Bridge(const rclcpp::NodeOptions &options = rclcpp::NodeOptions());
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__BRIDGE_HPP_
