// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The raw rcl servers' readiness as an executor wakeup: entity set in, a
// guard-condition trigger out, so a ROS goal, cancel, result or service
// request waits for no timer pass before the takes run. One helper thread
// blocks in rcl_wait() on the servers and touches nothing but its own
// state, because a raw rcl entity cannot join the executor's wait set the
// way an rclcpp entity can.
#ifndef R2S_BRIDGE__SERVER_WAKER_HPP_
#define R2S_BRIDGE__SERVER_WAKER_HPP_

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <rcl/service.h>
#include <rcl_action/action_server.h>
#include <rclcpp/rclcpp.hpp>

namespace r2s_bridge
{

/// Wakes the executor when a raw rcl server takes work.
class ServerWaker
{
public:
  /// Runs the takes when a goal, cancel, result or service request arrives.
  using ReadyCallback = std::function<void()>;

  /// Creates the waker and its helper thread.
  /**
   * \param[in] context the context the wait set and the wakeup guard live in.
   * \param[in] ready the takes to run, on the executor's thread.
   * \throws std::invalid_argument when the context is null or ready is empty.
   * \throws rclcpp::exceptions::RCLError when the wait set or the wakeup
   * guard cannot be made, and std::system_error when the thread cannot start.
   */
  ServerWaker(const rclcpp::Context::SharedPtr & context, ReadyCallback ready);

  /// Disarms, stops the helper thread and releases the wait set.
  ~ServerWaker();

  /// Not copyable.
  ServerWaker(const ServerWaker &) = delete;

  /// Not assignable.
  ServerWaker & operator=(const ServerWaker &) = delete;

  /// Watches servers, or disarms with empty sets.
  /**
   * The servers stay owned by the caller: watch() returns only after the
   * helper thread left the wait, and unwatch() is what lets the caller
   * destroy a watched server. Watching two empty sets parks the helper
   * until the next watch.
   *
   * \param[in] actions the action servers to watch, empty to watch none.
   * \param[in] services the service servers to watch, empty to watch none.
   */
  void watch(
    const std::vector<const rcl_action_server_t *> & actions,
    const std::vector<const rcl_service_t *> & services);

  /// Stops watching, waiting until the helper thread left the wait.
  /**
   * Must run before a watched server is destroyed: once this returns, the
   * helper waits on nothing but its own wakeup guard.
   */
  void unwatch();

  /// A transmit ran the takes: lets one ready server wake again.
  /**
   * The takes drain the entities, so a transmit that took something clears
   * the edge latch and the next arrival wakes. A transmit that took nothing
   * leaves the latch, so a persistently ready server degrades to the timer
   * takes instead of spinning the executor.
   *
   * \param[in] took whether the transmit took a request.
   */
  void notify_served(bool took);

private:
  void run();
  void rebuild(
    const std::vector<const rcl_action_server_t *> & actions,
    const std::vector<const rcl_service_t *> & services);
  bool query_ready(
    const std::vector<const rcl_action_server_t *> & actions,
    const std::vector<const rcl_service_t *> & services);

  ReadyCallback ready_;
  rclcpp::Context::SharedPtr context_;
  bool pending_ = false;
  bool signalled_ = false;
  std::thread thread_;

  mutable std::mutex mutex_;
  std::condition_variable idle_;
  std::vector<const rcl_action_server_t *> actions_;
  std::vector<const rcl_service_t *> services_;
  bool armed_ = false;
  bool quit_ = false;
  bool in_wait_ = false;
  rcl_wait_set_t wait_set_ = rcl_get_zero_initialized_wait_set();
  bool set_live_ = false;
  rcl_guard_condition_t wake_guard_ = rcl_get_zero_initialized_guard_condition();
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__SERVER_WAKER_HPP_
