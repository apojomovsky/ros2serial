// The host bridge node, one instance per serial port. Parameters are declared
// on construction so `ros2 param` can list and set them before a link exists;
// a Router owned here moves the traffic, polled on a short wall timer.
#ifndef R2S_BRIDGE__BRIDGE_HPP_
#define R2S_BRIDGE__BRIDGE_HPP_

#include <chrono>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>

#include <diagnostic_msgs/msg/diagnostic_array.hpp>

#include "r2s_bridge/router.hpp"
#include "r2s_bridge/steady_clock.hpp"

namespace r2s_bridge
{

class Bridge : public rclcpp::Node
{
  public:
    explicit Bridge(const rclcpp::NodeOptions &options = rclcpp::NodeOptions());
    ~Bridge() override;

  private:
    SteadyClock clock_;
    std::unique_ptr<Router> router_;
    rclcpp::TimerBase::SharedPtr timer_;
    rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_pub_;
    std::string diagnostics_hardware_id_;
    std::chrono::nanoseconds last_diagnostics_{0};
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__BRIDGE_HPP_
