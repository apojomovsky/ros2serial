// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// server waker: waited-on servers in, an executor wakeup out.
#include "r2s_bridge/server_waker.hpp"

#include <rcl_action/wait.h>

#include <cstdint>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace r2s_bridge
{

ServerWaker::ServerWaker(const rclcpp::Context::SharedPtr & context, ReadyCallback ready)
: ready_(std::move(ready)), context_(context)
{
  if (!context_) {
    throw std::invalid_argument("ServerWaker: a context is required");
  }
  if (!ready_) {
    throw std::invalid_argument("ServerWaker: a ready callback is required");
  }
  const rcl_guard_condition_options_t options = rcl_guard_condition_get_default_options();
  if (
    rcl_guard_condition_init(
      &wake_guard_, context_->get_rcl_context().get(), options) != RCL_RET_OK)
  {
    rcl_reset_error();
    // GCOVR_EXCL_START: a context that cannot spare one guard condition
    // leaves the node without any executor either, which no test stages.
    throw std::runtime_error("ServerWaker: cannot make the wakeup guard");
    // GCOVR_EXCL_STOP
  }
  try {
    thread_ = std::thread([this]() {run();});
  } catch (...) {
    [[maybe_unused]] const rcl_ret_t fini = rcl_guard_condition_fini(&wake_guard_);
    (void)fini;
    throw;
  }
}

ServerWaker::~ServerWaker()
{
  {
    std::lock_guard<std::mutex> guard(mutex_);
    quit_ = true;
  }
  [[maybe_unused]] const rcl_ret_t triggered = rcl_trigger_guard_condition(&wake_guard_);
  (void)triggered;
  idle_.notify_all();
  thread_.join();
  if (set_live_) {
    [[maybe_unused]] const rcl_ret_t wait_fini = rcl_wait_set_fini(&wait_set_);
    (void)wait_fini;
  }
  [[maybe_unused]] const rcl_ret_t guard_fini = rcl_guard_condition_fini(&wake_guard_);
  (void)guard_fini;
}

void ServerWaker::watch(
  const std::vector<const rcl_action_server_t *> & actions,
  const std::vector<const rcl_service_t *> & services)
{
  unwatch();
  {
    std::lock_guard<std::mutex> guard(mutex_);
    actions_ = actions;
    services_ = services;
    armed_ = true;
  }
  [[maybe_unused]] const rcl_ret_t triggered = rcl_trigger_guard_condition(&wake_guard_);
  (void)triggered;
  idle_.notify_all();
}

void ServerWaker::unwatch()
{
  std::unique_lock<std::mutex> lock(mutex_);
  armed_ = false;
  actions_.clear();
  services_.clear();
  pending_ = false;
  signalled_ = false;
  // A destroy the caller stages needs the helper out of the wait first: the
  // wakeup above returns it, and the predicate below waits for a helper that
  // was already rebuilding.
  const bool waiting = in_wait_;
  lock.unlock();
  [[maybe_unused]] const rcl_ret_t triggered = rcl_trigger_guard_condition(&wake_guard_);
  (void)triggered;
  lock.lock();
  if (waiting) {
    // Only this class notifies idle_, and it does so after clearing
    // in_wait_ so the predicate never reads false after a spurious wakeup.
    // GCOVR_EXCL_BR_START: the false edge needs a notify no other thread
    // sends, so no test can reach it.
    idle_.wait(lock, [this] {return !in_wait_ || quit_;});
    // GCOVR_EXCL_BR_STOP
  }
}

void ServerWaker::notify_served(bool took)
{
  {
    std::lock_guard<std::mutex> guard(mutex_);
    pending_ = false;
    if (took) {
      signalled_ = false;
    }
  }
  idle_.notify_all();
}

void ServerWaker::rebuild(
  const std::vector<const rcl_action_server_t *> & actions,
  const std::vector<const rcl_service_t *> & services)
{
  if (set_live_) {
    [[maybe_unused]] const rcl_ret_t fini = rcl_wait_set_fini(&wait_set_);
    (void)fini;
    set_live_ = false;
  }
  wait_set_ = rcl_get_zero_initialized_wait_set();
  std::size_t subs = 0u;
  std::size_t guards = 1u;
  std::size_t timers = 0u;
  std::size_t clients = 0u;
  std::size_t service_slots = services.size();
  for (const rcl_action_server_t * server : actions) {
    std::size_t s = 0u;
    std::size_t g = 0u;
    std::size_t t = 0u;
    std::size_t c = 0u;
    std::size_t sv = 0u;
    if (rcl_action_server_wait_set_get_num_entities(server, &s, &g, &t, &c, &sv) != RCL_RET_OK) {
      rcl_reset_error();
      throw std::runtime_error("ServerWaker: cannot size the wait set");
    }
    subs += s;
    guards += g;
    timers += t;
    clients += c;
    service_slots += sv;
  }
  rcl_allocator_t allocator = rcl_get_default_allocator();
  if (
    rcl_wait_set_init(
      &wait_set_, subs, guards, timers, clients, service_slots, 0u,
      context_->get_rcl_context().get(), allocator) != RCL_RET_OK)
  {
    rcl_reset_error();
    throw std::runtime_error("ServerWaker: cannot make the wait set");
  }
  set_live_ = true;
  if (rcl_wait_set_add_guard_condition(&wait_set_, &wake_guard_, nullptr) != RCL_RET_OK) {
    rcl_reset_error();
    throw std::runtime_error("ServerWaker: cannot watch its wakeup guard");
  }
  for (const rcl_action_server_t * server : actions) {
    if (rcl_action_wait_set_add_action_server(&wait_set_, server, nullptr) != RCL_RET_OK) {
      rcl_reset_error();
      throw std::runtime_error("ServerWaker: cannot watch an action server");
    }
  }
  for (const rcl_service_t * service : services) {
    std::size_t index = 0u;
    if (rcl_wait_set_add_service(&wait_set_, service, &index) != RCL_RET_OK) {
      rcl_reset_error();
      throw std::runtime_error("ServerWaker: cannot watch a service server");
    }
  }
}

bool ServerWaker::query_ready(
  const std::vector<const rcl_action_server_t *> & actions,
  const std::vector<const rcl_service_t *> & services)
{
  for (const rcl_action_server_t * server : actions) {
    bool goal = false;
    bool cancel = false;
    bool result = false;
    bool expired = false;
    if (
      rcl_action_server_wait_set_get_entities_ready(
        &wait_set_, server, &goal, &cancel, &result, &expired) != RCL_RET_OK)
    {
      rcl_reset_error();
      return false;
    }
    if (goal || cancel || result) {
      return true;
    }
  }
  // A waited service stays in the set exactly while it is ready: the wait
  // prunes the rest, so membership is the readiness.
  for (std::size_t i = 0u; i < wait_set_.size_of_services; ++i) {
    for (const rcl_service_t * service : services) {
      if (wait_set_.services[i] == service) {
        return true;
      }
    }
  }
  return false;
}

void ServerWaker::run()
{
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    idle_.wait(lock, [this] {return quit_ || (armed_ && !pending_ && !signalled_);});
    if (quit_) {
      return;
    }
    const std::vector<const rcl_action_server_t *> actions = actions_;
    const std::vector<const rcl_service_t *> watched = services_;
    in_wait_ = true;
    lock.unlock();
    // The set is rebuilt every pass, as the executor's own loop does: the
    // wait consumes every entry it returns, so a reused set would miss the
    // servers the last pass pruned.
    try {
      rebuild(actions, watched);
    } catch (const std::runtime_error &) {
      // A rebuild fails only on a dead server or a refused allocation, and
      // the next transmit clears pending_: the waiter retries at the timer
      // rate instead of spinning or dying with the thread.
      // GCOVR_EXCL_START: no test can refuse the middleware an allocation.
      lock.lock();
      pending_ = true;
      in_wait_ = false;
      idle_.notify_all();
      continue;
      // GCOVR_EXCL_STOP
    }
    [[maybe_unused]] const rcl_ret_t waited = rcl_wait(&wait_set_, -1);
    (void)waited;
    lock.lock();
    in_wait_ = false;
    idle_.notify_all();
    if (quit_) {
      return;
    }
    if (!armed_) {
      continue;
    }
    const bool ready = query_ready(actions, watched);
    if (!ready) {
      signalled_ = false;
      continue;
    }
    if (!pending_ && !signalled_) {
      // The takes run on the executor, so the ready flag stays until the
      // transmit drains it: pending_ parks the helper, and signalled_
      // stops a still-ready server from waking the executor again. The
      // callback runs unlocked, the way the link waitable executes.
      pending_ = true;
      signalled_ = true;
      lock.unlock();
      ready_();
      lock.lock();
    }
  }
}

}  // namespace r2s_bridge
