// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The link waitable: a pty stands in for the port, and each test drives one
// wait set by hand instead of an executor, so what it asserts is the wakeup
// itself rather than a callback that a poll loop could have produced.
#include <gtest/gtest.h>

#include <pty.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <stdexcept>
#include <string>

#include <rcl/rcl.h>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp/version.h>

#include "r2s_bridge/link_waitable.hpp"
#include "r2s_bridge/termios_transport.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

// A pty pair the test writes to like the MCU side: the transport owns the
// slave and the test holds the master.
class Pty
{
public:
  Pty()
  {
    if (openpty(&master_, &slave_, nullptr, nullptr, nullptr) != 0) {
      throw std::runtime_error(std::string("openpty failed: ") + std::strerror(errno));
    }
    char name[256]{};
    if (ptsname_r(master_, name, sizeof name) != 0) {
      throw std::runtime_error(std::string("ptsname_r failed: ") + std::strerror(errno));
    }
    name_ = name;
  }
  ~Pty()
  {
    if (master_ >= 0) {
      ::close(master_);
    }
    if (slave_ >= 0) {
      ::close(slave_);
    }
  }
  Pty(const Pty &) = delete;
  Pty & operator=(const Pty &) = delete;

  const std::string & name() const {return name_;}
  int master() const {return master_;}
  void close_master()
  {
    if (master_ >= 0) {
      ::close(master_);
      master_ = -1;
    }
  }
  void write_byte()
  {
    const std::uint8_t byte = 0x5a;
    if (::write(master_, &byte, 1) != 1) {
      throw std::runtime_error(std::string("pty write failed: ") + std::strerror(errno));
    }
  }

private:
  int master_ = -1;
  int slave_ = -1;
  std::string name_;
};

class LinkWaitableTest : public ::testing::Test
{
protected:
  static void SetUpTestSuite()
  {
    rclcpp::init(0, nullptr);
  }
  static void TearDownTestSuite()
  {
    rclcpp::shutdown();
  }
};

// One bounded wait on a wait set holding the waitable's guard condition. The
// wait set is cleared and refilled every time, as the executor's rebuild does,
// so the arrays never carry a stale entry between waits.
rcl_ret_t wait_once(rcl_wait_set_t & wait_set, LinkWaitable & waitable, std::chrono::nanoseconds how)
{
  rcl_ret_t status = rcl_wait_set_clear(&wait_set);
  if (status != RCL_RET_OK) {
    return status;
  }
  waitable.add_to_wait_set(wait_set);
  return rcl_wait(&wait_set, how.count());
}

TEST_F(LinkWaitableTest, WakesWhenThePeerWrites)
{
  Pty pty;
  TermiosTransport transport(TermiosOptions{pty.name()});
  int ready = 0;
  LinkWaitable waitable(
    rclcpp::contexts::get_global_default_context(), [&ready]() {++ready;});
  ASSERT_GE(transport.native_handle(), 0) << "an open port has a descriptor to watch";
  waitable.watch(transport.native_handle());

  rcl_wait_set_t wait_set = rcl_get_zero_initialized_wait_set();
  ASSERT_EQ(
    rcl_wait_set_init(&wait_set, 0, 1, 0, 0, 0, 0,
    rclcpp::contexts::get_global_default_context()->get_rcl_context().get(),
    rcl_get_default_allocator()), RCL_RET_OK);
  ASSERT_EQ(wait_once(wait_set, waitable, std::chrono::milliseconds(50)), RCL_RET_TIMEOUT)
    << "a quiet line must not wake the executor";

  pty.write_byte();

  ASSERT_EQ(wait_once(wait_set, waitable, std::chrono::seconds(5)), RCL_RET_OK);
  EXPECT_TRUE(waitable.is_ready(wait_set));
  EXPECT_EQ(ready, 0) << "the wakeup runs nothing until the executor executes it";
  waitable.execute(waitable.take_data());
  EXPECT_EQ(ready, 1);
  EXPECT_EQ(rcl_wait_set_fini(&wait_set), RCL_RET_OK);
}

TEST_F(LinkWaitableTest, WakesAgainAfterTheExecuteAcknowledges)
{
  Pty pty;
  TermiosTransport transport(TermiosOptions{pty.name()});
  int ready = 0;
  LinkWaitable waitable(
    rclcpp::contexts::get_global_default_context(), [&ready]() {++ready;});
  waitable.watch(transport.native_handle());
  rcl_wait_set_t wait_set = rcl_get_zero_initialized_wait_set();
  ASSERT_EQ(
    rcl_wait_set_init(&wait_set, 0, 1, 0, 0, 0, 0,
    rclcpp::contexts::get_global_default_context()->get_rcl_context().get(),
    rcl_get_default_allocator()), RCL_RET_OK);

  pty.write_byte();
  ASSERT_EQ(wait_once(wait_set, waitable, std::chrono::seconds(5)), RCL_RET_OK);
  waitable.execute(waitable.take_data());
  // The bytes stay unread: the second wakeup must still come, so the executor
  // reads them on the next pass instead of waiting for the next timer tick.
  pty.write_byte();
  ASSERT_EQ(wait_once(wait_set, waitable, std::chrono::seconds(5)), RCL_RET_OK);

  waitable.execute(waitable.take_data());
  EXPECT_EQ(ready, 2);
  EXPECT_EQ(rcl_wait_set_fini(&wait_set), RCL_RET_OK);
}

TEST_F(LinkWaitableTest, WakesOnHangup)
{
  Pty pty;
  TermiosTransport transport(TermiosOptions{pty.name()});
  LinkWaitable waitable(rclcpp::contexts::get_global_default_context(), []() {});
  waitable.watch(transport.native_handle());
  rcl_wait_set_t wait_set = rcl_get_zero_initialized_wait_set();
  ASSERT_EQ(
    rcl_wait_set_init(&wait_set, 0, 1, 0, 0, 0, 0,
    rclcpp::contexts::get_global_default_context()->get_rcl_context().get(),
    rcl_get_default_allocator()), RCL_RET_OK);

  pty.close_master();

  ASSERT_EQ(wait_once(wait_set, waitable, std::chrono::seconds(5)), RCL_RET_OK)
    << "a hung-up line is readable, and the read path reports the fault";
  EXPECT_TRUE(waitable.is_ready(wait_set));
  EXPECT_EQ(rcl_wait_set_fini(&wait_set), RCL_RET_OK);
}

TEST_F(LinkWaitableTest, StopsWakingAfterUnwatch)
{
  Pty pty;
  TermiosTransport transport(TermiosOptions{pty.name()});
  int ready = 0;
  LinkWaitable waitable(
    rclcpp::contexts::get_global_default_context(), [&ready]() {++ready;});
  waitable.watch(transport.native_handle());
  waitable.unwatch();
  rcl_wait_set_t wait_set = rcl_get_zero_initialized_wait_set();
  ASSERT_EQ(
    rcl_wait_set_init(&wait_set, 0, 1, 0, 0, 0, 0,
    rclcpp::contexts::get_global_default_context()->get_rcl_context().get(),
    rcl_get_default_allocator()), RCL_RET_OK);

  pty.write_byte();

  EXPECT_EQ(wait_once(wait_set, waitable, std::chrono::milliseconds(100)), RCL_RET_TIMEOUT)
    << "a disarmed waitable must not wake on a descriptor it no longer owns";
  EXPECT_EQ(ready, 0);
  EXPECT_EQ(rcl_wait_set_fini(&wait_set), RCL_RET_OK);
}

// The fake transport has no descriptor, so a link without readiness support
// keeps today's timer-only receive and the waitable stays inert.
TEST_F(LinkWaitableTest, AWatchWithoutADescriptorNeverFires)
{
  int ready = 0;
  LinkWaitable waitable(
    rclcpp::contexts::get_global_default_context(), [&ready]() {++ready;});
  waitable.watch(-1);
  rcl_wait_set_t wait_set = rcl_get_zero_initialized_wait_set();
  ASSERT_EQ(
    rcl_wait_set_init(&wait_set, 0, 1, 0, 0, 0, 0,
    rclcpp::contexts::get_global_default_context()->get_rcl_context().get(),
    rcl_get_default_allocator()), RCL_RET_OK);

  EXPECT_EQ(wait_once(wait_set, waitable, std::chrono::milliseconds(50)), RCL_RET_TIMEOUT);
  EXPECT_EQ(ready, 0);
  EXPECT_EQ(rcl_wait_set_fini(&wait_set), RCL_RET_OK);
}

// The point of the class is the executor: added to a node, a byte on the
// port must run the callback on the executor's own thread, with no timer and
// no other entity to produce the wakeup.
TEST_F(LinkWaitableTest, AByteRunsTheCallbackOnTheExecutor)
{
  Pty pty;
  TermiosTransport transport(TermiosOptions{pty.name()});
  int ready = 0;
  auto waitable = std::make_shared<LinkWaitable>(
    rclcpp::contexts::get_global_default_context(), [&ready]() {++ready;});
  waitable->watch(transport.native_handle());
  auto node = std::make_shared<rclcpp::Node>("link_waitable_probe");
  auto group = node->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
  node->get_node_waitables_interface()->add_waitable(waitable, group);
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(node);

  EXPECT_EQ(waitable->watched(), transport.native_handle());
  // The byte lands while the executor waits, which is the production order:
  // a wakeup only exists for the wait it interrupts.
  std::thread writer([&pty]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      pty.write_byte();
    });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (ready == 0 && std::chrono::steady_clock::now() < deadline) {
    executor.spin_once(std::chrono::milliseconds(50));
  }
  writer.join();

  EXPECT_EQ(ready, 1) << "the executor must run the receive path on the byte's wakeup";
  node->get_node_waitables_interface()->remove_waitable(waitable, group);
}

TEST_F(LinkWaitableTest, ArmsAgainAfterAnotherWatch)
{
  Pty pty;
  TermiosTransport transport(TermiosOptions{pty.name()});
  LinkWaitable waitable(rclcpp::contexts::get_global_default_context(), []() {});
  waitable.watch(transport.native_handle());
  waitable.unwatch();
  waitable.watch(transport.native_handle());
  rcl_wait_set_t wait_set = rcl_get_zero_initialized_wait_set();
  ASSERT_EQ(
    rcl_wait_set_init(&wait_set, 0, 1, 0, 0, 0, 0,
    rclcpp::contexts::get_global_default_context()->get_rcl_context().get(),
    rcl_get_default_allocator()), RCL_RET_OK);

  pty.write_byte();

  ASSERT_EQ(wait_once(wait_set, waitable, std::chrono::seconds(5)), RCL_RET_OK);
  EXPECT_TRUE(waitable.is_ready(wait_set));
  EXPECT_EQ(rcl_wait_set_fini(&wait_set), RCL_RET_OK);
}

// A waitable with nothing to run would swallow every wakeup and leave the
// link silent with no error, so the constructor refuses it.
TEST_F(LinkWaitableTest, RefusesAWaitableWithNoCallback)
{
  EXPECT_THROW(
    LinkWaitable(rclcpp::contexts::get_global_default_context(), LinkWaitable::ReadyCallback()),
    std::invalid_argument);
}

// A disarm with the helper already idle returns at once: it must not block on
// a poll that is not running, which is the state an armed-then-idle link is
// in when the port closes.
TEST_F(LinkWaitableTest, UnwatchReturnsWhenTheHelperIsIdle)
{
  Pty pty;
  TermiosTransport transport(TermiosOptions{pty.name()});
  LinkWaitable waitable(rclcpp::contexts::get_global_default_context(), []() {});
  waitable.watch(transport.native_handle());
  waitable.unwatch();
  waitable.unwatch();
  EXPECT_EQ(waitable.watched(), -1);
}

// The disarm that matters: the helper is blocked in poll() when the port
// closes, so unwatch must interrupt it and wait for it to leave before the
// descriptor goes away. A wait that returned early would leave the helper
// polling a number the next open hands to somebody else.
TEST_F(LinkWaitableTest, UnwatchInterruptsAHelperParkedInPoll)
{
  Pty pty;
  TermiosTransport transport(TermiosOptions{pty.name()});
  LinkWaitable waitable(rclcpp::contexts::get_global_default_context(), []() {});
  waitable.watch(transport.native_handle());
  // The helper enters poll() as soon as it sees the watch; this lets it get
  // there before the disarm, which is the ordering the port close takes.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  const auto started = std::chrono::steady_clock::now();
  waitable.unwatch();
  const auto took = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(waitable.watched(), -1);
  EXPECT_LT(took, std::chrono::seconds(2)) << "the disarm must not wait for a byte";
}

// A watch that replaces one descriptor with another while the helper sleeps
// in poll() on the first: the old readiness must be discarded, or the next
// link would inherit a wakeup that belonged to its predecessor.
TEST_F(LinkWaitableTest, AReplacedDescriptorDiscardsTheOldReadiness)
{
  Pty first;
  Pty second;
  TermiosTransport first_transport(TermiosOptions{first.name()});
  TermiosTransport second_transport(TermiosOptions{second.name()});
  int ready = 0;
  LinkWaitable waitable(
    rclcpp::contexts::get_global_default_context(), [&ready]() {++ready;});
  waitable.watch(first_transport.native_handle());
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // The first link has bytes when the swap lands, so its readiness is the
  // one the helper sees and must discard.
  first.write_byte();
  waitable.watch(second_transport.native_handle());
  second.write_byte();
  rcl_wait_set_t wait_set = rcl_get_zero_initialized_wait_set();
  ASSERT_EQ(
    rcl_wait_set_init(&wait_set, 0, 1, 0, 0, 0, 0,
    rclcpp::contexts::get_global_default_context()->get_rcl_context().get(),
    rcl_get_default_allocator()), RCL_RET_OK);

  ASSERT_EQ(wait_once(wait_set, waitable, std::chrono::seconds(5)), RCL_RET_OK);
  waitable.execute(waitable.take_data());
  EXPECT_EQ(ready, 1) << "the second link's byte is what wakes the executor";
  EXPECT_EQ(waitable.watched(), second_transport.native_handle());
  EXPECT_EQ(rcl_wait_set_fini(&wait_set), RCL_RET_OK);
}

// The events-executor surface the newer rclcpp requires: one entity, so
// every id takes the same empty data, every readiness reports one wakeup
// of entity zero, and no timers live in the waitable.
TEST_F(LinkWaitableTest, EventsExecutorSurfaceReportsOneEntity)
{
  LinkWaitable waitable(rclcpp::contexts::get_global_default_context(), []() {});
  EXPECT_EQ(waitable.take_data_by_entity_id(0u), nullptr);
  EXPECT_EQ(waitable.take_data_by_entity_id(7u), nullptr);
  std::size_t calls = 0u;
  std::size_t count = 0u;
  int id = -1;
  waitable.set_on_ready_callback([&calls, &count, &id](std::size_t n, int entity) {
    ++calls;
    count = n;
    id = entity;
  });
  waitable.trigger();
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(id, 0);
  waitable.clear_on_ready_callback();
  waitable.trigger();
  EXPECT_EQ(calls, 1u) << "a cleared callback must not run";
#if RCLCPP_VERSION_MAJOR >= 29
  EXPECT_TRUE(waitable.get_timers().empty());
#endif
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge
