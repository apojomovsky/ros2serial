// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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

#include "r2s_bridge/ros_clock.hpp"
#include "r2s_bridge/router.hpp"
#include "r2s_bridge/steady_clock.hpp"

namespace r2s_bridge
{

class Bridge : public rclcpp::Node
{
public:
  explicit Bridge(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());
  ~Bridge() override;

    // The poll entry the wall timer calls: polls the router and emits one
    // /diagnostics message when the period elapses. Public so tests drive it
    // without waiting on the timer.
  void poll();

protected:
    // Publishes one /diagnostics message. Virtual so tests inject a failure.
  virtual void publish_diagnostics(const diagnostic_msgs::msg::DiagnosticArray & message);

private:
  SteadyClock clock_;
  RosClock ros_clock_;
  std::unique_ptr<Router> router_;
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_pub_;
  std::string diagnostics_hardware_id_;
  std::chrono::nanoseconds last_diagnostics_{0};
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__BRIDGE_HPP_
