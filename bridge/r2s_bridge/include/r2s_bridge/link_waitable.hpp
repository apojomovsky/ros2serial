// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The link's readiness as an executor entity: a descriptor in, a guard
// condition out, so a byte on the port wakes the executor instead of waiting
// for the next timer pass. One helper thread blocks in poll() and touches
// nothing but its own state, because the rmw wait sets do not offer a
// portable descriptor slot.
#ifndef R2S_BRIDGE__LINK_WAITABLE_HPP_
#define R2S_BRIDGE__LINK_WAITABLE_HPP_

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <rclcpp/rclcpp.hpp>

namespace r2s_bridge
{

/// Wakes the executor when the serial descriptor turns readable.
/**
 * The executor waits on one guard condition this class owns; a helper thread
 * blocks in poll() on that condition's wakeup pipe and the link's
 * descriptor, and triggers the condition when the link has bytes or hung up.
 * The ready callback runs on the executor's thread, never on the helper's,
 * so the session keeps its single-threaded contract. A watch() of -1 keeps
 * the class inert, which is what a transport with no descriptor gets:
 * exactly the timer-only receive of before.
 */
class LinkWaitable : public rclcpp::Waitable
{
public:
  /// Runs the receive path when the link turns readable, on the executor.
  using ReadyCallback = std::function<void()>;

  /// Creates the waitable and its helper thread.
  /**
   * \param[in] context the context whose guard condition is waited on.
   * \param[in] ready the receive path to run, on the executor's thread.
   * \throws rclcpp::exceptions::RCLError when the guard condition cannot be
   * made, and std::system_error when the wakeup pipe cannot.
   */
  LinkWaitable(const rclcpp::Context::SharedPtr & context, ReadyCallback ready);

  /// Disarms, stops the helper thread and releases the guard condition.
  ~LinkWaitable() override;

  /// Not copyable.
  LinkWaitable(const LinkWaitable &) = delete;

  /// Not assignable.
  LinkWaitable & operator=(const LinkWaitable &) = delete;

  /// Watches a descriptor, or disarms with -1.
  /**
   * The previous descriptor is dropped without closing it: whoever opened it
   * closes it, and unwatch() has already made sure the helper thread no
   * longer reads that number by the time it returns. -1 disarms the same
   * way and leaves the executor's timer the only reader.
   *
   * \param[in] fd the descriptor to watch, or -1 to stop watching.
   */
  void watch(int fd);

  /// Wakes the executor without touching the descriptor.
  /**
   * A ROS-side arrival runs on the executor already, so it cannot wait for
   * the helper thread: it fires the guard condition directly, and the next
   * wait returns with is_ready() true. Thread-safe, like the helper's own
   * wakeups.
   */
  void trigger();

  /// Stops watching, waiting until the helper thread left the descriptor.
  /**
   * Must run before the watched descriptor is closed: once this returns, the
   * helper thread polls nothing but its own wakeup pipe.
   */
  void unwatch();

  /// Adds this waitable's guard condition to the wait set.
  /**
   * \param[in] wait_set the wait set to add to.
   */
  void add_to_wait_set(rcl_wait_set_t & wait_set) override;

  /// Whether the guard condition fired in this wait.
  /**
   * \param[in] wait_set the wait set the waitable was added to and waited on.
   * \return True when the condition is set in that wait set.
   */
  bool is_ready(const rcl_wait_set_t & wait_set) override;

  /// Nothing to take: the work is the descriptor, read by the callback.
  /**
   * \return Always null.
   */
  std::shared_ptr<void> take_data() override;

  /// Runs the ready callback and arms the next wakeup.
  /**
   * The condition is rearmed here rather than when it fires, so a byte that
   * arrives during the callback is still noticed.
   *
   * \param[in] data ignored.
   */
  void execute(const std::shared_ptr<void> & data) override;

  /// The descriptor this waitable waits on, or -1 when disarmed.
  /**
   * It is the session's open port, so a test asserts the node wired the one
   * it opened rather than a number from somewhere else.
   *
   * \return The watched descriptor, or -1.
   */
  int watched() const;

  /// The guard condition this waitable contributes.
  /**
   * \return Always one.
   */
  std::size_t get_number_of_ready_guard_conditions() override;

private:
  void run();
  void wake();
  void drain_wake();
  void notify_ready();

  rclcpp::GuardCondition::SharedPtr guard_;
  ReadyCallback ready_;
  bool pending_ = false;
  std::thread thread_;

  mutable std::mutex mutex_;
  std::condition_variable idle_;
  int fd_ = -1;
  bool armed_ = false;
  bool quit_ = false;
  bool in_poll_ = false;
  int wake_read_ = -1;
  int wake_write_ = -1;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__LINK_WAITABLE_HPP_
