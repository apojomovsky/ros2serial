// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

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

/// The host bridge node, one instance per serial port.
/**
 * Parameters are declared on construction so `ros2 param` can list and set
 * them before a link exists; a Router owned here moves the traffic, polled
 * on a short wall timer.
 */
class Bridge : public rclcpp::Node
{
public:
  /// Starts the node and declares its parameters.
  /**
   * \param[in] options the node options.
   */
  explicit Bridge(const rclcpp::NodeOptions & options = rclcpp::NodeOptions());

  /// Destroys the node and its router.
  ~Bridge() override;

  /// The poll entry the wall timer calls.
  /**
   * Polls the router and emits one /diagnostics message when the period
   * elapses. Public so tests drive it without waiting on the timer.
   */
  void poll();

protected:
  /// Publishes one /diagnostics message.
  /**
   * Virtual so tests inject a failure.
   *
   * \param[in] message the array to publish.
   */
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
