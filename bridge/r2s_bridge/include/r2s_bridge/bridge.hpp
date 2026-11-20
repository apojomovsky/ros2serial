// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The host bridge node, one instance per serial port. Parameters are declared
// on construction so `ros2 param` can list and set them before a link exists;
// a Router owned here moves the traffic, woken by the port's readiness and
// polled on a wall timer for the pacer and the heartbeats.
#ifndef R2S_BRIDGE__BRIDGE_HPP_
#define R2S_BRIDGE__BRIDGE_HPP_

#include <chrono>
#include <memory>
#include <string>

#include <rclcpp/rclcpp.hpp>

#include <diagnostic_msgs/msg/diagnostic_array.hpp>

#include "r2s_bridge/link_waitable.hpp"
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

  /// The readiness entity the executor waits on beside the timer.
  /**
   * One per node: it watches the port the router opened and runs the
   * receive half when a byte arrives. Public so a test proves the node
   * wired its port to it.
   *
   * \return The waitable, never null.
   */
  const std::shared_ptr<LinkWaitable> & link_waitable() const;

protected:
  /// Publishes one /diagnostics message.
  /**
   * Virtual so tests inject a failure.
   *
   * \param[in] message the array to publish.
   */
  virtual void publish_diagnostics(const diagnostic_msgs::msg::DiagnosticArray & message);

private:
  /// Tracks the port the session opened, for the waitable.
  /**
   * The session publishes the descriptor on every open and -1 on every
   * close, on the thread that polls it, so the waitable never watches a
   * number the session already handed to somebody else.
   *
   * \param[in] fd the open port's descriptor, or -1.
   */
  void on_port(int fd);

  /// Runs the receive half when the port turns readable.
  void on_readable();

  SteadyClock clock_;
  RosClock ros_clock_;
  std::unique_ptr<Router> router_;
  std::shared_ptr<LinkWaitable> link_;
  rclcpp::CallbackGroup::SharedPtr link_group_;
  int link_fd_ = -1;
  rclcpp::TimerBase::SharedPtr timer_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_pub_;
  std::string diagnostics_hardware_id_;
  std::chrono::nanoseconds last_diagnostics_{0};
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__BRIDGE_HPP_
