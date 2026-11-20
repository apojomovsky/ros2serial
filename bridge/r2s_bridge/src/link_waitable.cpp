// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// link waitable: a readable descriptor in, an executor wakeup out.
#include "r2s_bridge/link_waitable.hpp"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace r2s_bridge
{
namespace
{

void close_fd(int & fd)
{
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

}  // namespace

LinkWaitable::LinkWaitable(const rclcpp::Context::SharedPtr & context, ReadyCallback ready)
: guard_(std::make_shared<rclcpp::GuardCondition>(context)), ready_(std::move(ready))
{
  int pipe_fds[2] = {-1, -1};
  if (::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
    throw std::system_error(errno, std::generic_category(), "LinkWaitable: cannot make the wakeup pipe");
  }
  wake_read_ = pipe_fds[0];
  wake_write_ = pipe_fds[1];
  try {
    thread_ = std::thread([this]() {run();});
  } catch (...) {
    close_fd(wake_read_);
    close_fd(wake_write_);
    throw;
  }
}

LinkWaitable::~LinkWaitable()
{
  {
    std::lock_guard<std::mutex> guard(mutex_);
    quit_ = true;
    armed_ = false;
  }
  wake();
  idle_.notify_all();
  if (thread_.joinable()) {
    thread_.join();
  }
  close_fd(wake_read_);
  close_fd(wake_write_);
}

void LinkWaitable::watch(int fd)
{
  unwatch();
  if (fd < 0) {
    return;
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    fd_ = fd;
    armed_ = true;
  }
  idle_.notify_all();
}

void LinkWaitable::unwatch()
{
  std::unique_lock<std::mutex> lock(mutex_);
  armed_ = false;
  fd_ = -1;
  // A wakeup the executor never acknowledged dies with the disarm: the next
  // watch() must be able to poll, not wait behind a stale flag.
  pending_ = false;
  if (!thread_.joinable() || quit_) {
    return;
  }
  // Wait until the helper leaves poll() before returning, so the caller may
  // close the descriptor the moment this does. The wakeup write happens once
  // the lock is dropped, since the helper needs it to notice the disarm.
  const bool polling = in_poll_;
  lock.unlock();
  wake();
  lock.lock();
  if (polling) {
    idle_.wait(lock, [this] {return !in_poll_ || quit_;});
  }
}

void LinkWaitable::wake()
{
  const std::uint8_t byte = 1u;
  // A full pipe already holds a wakeup the helper has not read: one is
  // enough, and a short write here only means the same thing.
  (void)::write(wake_write_, &byte, 1);
}

void LinkWaitable::drain_wake()
{
  std::uint8_t bytes[16];
  while (::read(wake_read_, bytes, sizeof bytes) > 0) {
  }
}

void LinkWaitable::notify_ready()
{
  try {
    guard_->trigger();
  } catch (const std::exception &) {
    // A context going away needs no wakeup: the executor is already leaving.
  }
}

void LinkWaitable::run()
{
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    idle_.wait(lock, [this] {return quit_ || (armed_ && !pending_);});
    if (quit_) {
      return;
    }
    const int fd = fd_;
    const int wake_fd = wake_read_;
    in_poll_ = true;
    lock.unlock();
    // No timeout: a quiet line costs nothing until a byte, a hangup or a
    // disarm arrives.
    pollfd fds[2] = {{wake_fd, POLLIN, 0}, {fd, POLLIN, 0}};
    const int ready_fds = ::poll(fds, 2u, -1);
    lock.lock();
    in_poll_ = false;
    idle_.notify_all();
    if (quit_) {
      return;
    }
    if (ready_fds > 0 && (fds[0].revents & POLLIN) != 0) {
      drain_wake();
    }
    // A disarm while poll() ran replaced the descriptor: its readiness says
    // nothing about the link the caller installed next.
    if (!armed_ || fd != fd_) {
      continue;
    }
    if (ready_fds > 0 && fds[1].revents != 0) {
      // The bytes stay in the kernel queue, so the descriptor reports
      // readable while the executor has not drained it: pending_ keeps the
      // helper from spinning on the same readiness until it does.
      pending_ = true;
      notify_ready();
    }
  }
}

void LinkWaitable::add_to_wait_set(rcl_wait_set_t & wait_set)
{
  const rcl_ret_t status = rcl_wait_set_add_guard_condition(
    &wait_set, &guard_->get_rcl_guard_condition(), nullptr);
  if (status != RCL_RET_OK) {
    throw std::runtime_error("LinkWaitable: cannot add its guard condition to the wait set");
  }
}

bool LinkWaitable::is_ready(const rcl_wait_set_t & wait_set)
{
  const rcl_guard_condition_t * mine = &guard_->get_rcl_guard_condition();
  for (std::size_t index = 0; index < wait_set.size_of_guard_conditions; ++index) {
    if (wait_set.guard_conditions[index] == mine) {
      return true;
    }
  }
  return false;
}

std::shared_ptr<void> LinkWaitable::take_data()
{
  return nullptr;
}

void LinkWaitable::execute(const std::shared_ptr<void> & data)
{
  (void)data;
  // The callback drains the link, so the next wakeup waits until it returns:
  // the helper stays parked while the executor is inside the receive path.
  if (ready_) {
    ready_();
  }
  {
    std::lock_guard<std::mutex> guard(mutex_);
    pending_ = false;
  }
  idle_.notify_all();
}

int LinkWaitable::watched() const
{
  std::lock_guard<std::mutex> guard(mutex_);
  return fd_;
}

std::size_t LinkWaitable::get_number_of_ready_guard_conditions()
{
  return 1u;
}

}  // namespace r2s_bridge
