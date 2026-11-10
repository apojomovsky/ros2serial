// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* The action server through the fake port: registration, the walk bytes,
 * the goal and cancel callbacks, feedback and the terminal results. The
 * fake action carries Float32 goals, results and feedback, so the wire form
 * is the one the handwritten descriptors pin. */

#include <float.h>
#include <stdio.h>
#include <string.h>

/* The fake port's header carries no C linkage guard, and this file is C++. */
extern "C" {
#include "fake_port.h"
}
#include "frames.h"
#include "handwritten.h"
#include "r2s/action.hpp"
#include "r2s/r2s.h"
#include "r2s/r2s_actions.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/runtime.hpp"
#include "unity.h"

typedef std_msgs__msg__Float32 Float32;

struct Spin
{
  using Goal = Float32;
  using Result = Float32;
  using Feedback = Float32;
};

static const uint8_t s_float_hash[8] = {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u};
static const uint8_t s_action_hash[8] = {0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u};

namespace r2s
{
template<>
struct msg_traits<Float32>
{
  static const r2s_codec_table_t * table()
  {
    return &R2S_HANDWRITTEN_TABLE;
  }
  static uint16_t type_index()
  {
    return R2S_HANDWRITTEN_FLOAT32;
  }
  static const char * type_name()
  {
    return "std_msgs/msg/Float32";
  }
  static const uint8_t * type_hash()
  {
    return s_float_hash;
  }
};

namespace action
{
template<>
struct traits<Spin>
{
  static const char * action_name()
  {
    return "spin";
  }
  static const char * type_name()
  {
    return "test_msgs/action/Fibonacci";
  }
  static const uint8_t * type_hash()
  {
    return s_action_hash;
  }
};
} // namespace action
} // namespace r2s

/* The standing callbacks: each test names the decision the layer returns. */
static Float32 s_goal_seen;
static uint8_t s_goal_calls;
static r2s::action::GoalResponse s_goal_decision;
static uint8_t s_cancel_calls;
static r2s::action::CancelResponse s_cancel_decision;
static r2s::action::ServerGoalHandle<Spin> *s_handle;
static uint8_t s_accepted_calls;

static r2s::action::GoalResponse on_goal(const Float32 & goal)
{
  s_goal_seen = goal;
  s_goal_calls++;
  return s_goal_decision;
}

static r2s::action::CancelResponse on_cancel(const Float32 & goal)
{
  (void)goal;
  s_cancel_calls++;
  return s_cancel_decision;
}

static void on_accepted(r2s::action::ServerGoalHandle<Spin> & handle)
{
  s_handle = &handle;
  s_accepted_calls++;
}

static void reset_calls(void)
{
  s_goal_seen.data = 0.0f;
  s_goal_calls = 0u;
  s_goal_decision = r2s::action::GoalResponse::ACCEPT_AND_EXECUTE;
  s_cancel_calls = 0u;
  s_cancel_decision = r2s::action::CancelResponse::ACCEPT;
  s_handle = NULL;
  s_accepted_calls = 0u;
}

/* Queues bytes one at a time, spinning after each, as a slow link delivers
 * them. */
static void feed_slow(const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0u; i < len; i++) {
    fake_port_queue_rx(&data[i], 1);
    r2s_spin_some();
  }
}

/* Builds a host frame and feeds it byte by byte. */
static void feed_host(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
  static uint8_t wire[R2S_FRAMER_RX_CAPACITY + 1u];
  uint16_t wire_len = r2s_test_frame(wire, topic_id, seq, payload, len);

  feed_slow(wire, wire_len);
}

/* The index-th captured frame against the wire form of its payload. */
static void assert_tx(
  uint16_t index, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
  uint16_t payload_len)
{
  static uint8_t expected[R2S_FRAMER_RX_CAPACITY + 1u];
  char label[64];
  uint16_t encoded_len;
  const uint8_t *encoded;
  uint16_t expected_len = r2s_test_frame(expected, topic_id, seq, payload, payload_len);

  snprintf(label, sizeof label, "tx frame %u", index);
  encoded = fake_port_tx_frame(index, &encoded_len);
  TEST_ASSERT_NOT_NULL_MESSAGE(encoded, label);
  TEST_ASSERT_EQUAL_UINT16(expected_len - 1u, encoded_len);
  TEST_ASSERT_EQUAL_MEMORY(expected, encoded, expected_len - 1u);
}

/* One Float32 packed the way the codec packs it. */
static uint16_t pack_float(float value, uint8_t *out)
{
  Float32 msg;
  r2s_codec_out_t sink = {out, R2S_MAX_FRAME, 0u, 0u};
  uint8_t field = 0xFFu;

  msg.data = value;
  TEST_ASSERT_EQUAL_INT8(
        R2S_OK,
    r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_FLOAT32, &msg, 0u, &sink, &field));
  return sink.len;
}

/* Starts the node, answers the walk for the bound action and connects. */
static void connect_resolved(r2s::Node & node)
{
  const uint8_t query[] = {0x01u, 0x00u};
  const uint8_t verdict[] = {0x03u, 0x00u, 0x00u, 0x00u};
  const uint8_t ready[] = {0x06u, 0x01u, 0x00u, 0x00u, 0x00u};

  node.spin_some();
  feed_host(8, 0, query, sizeof query);
  feed_host(8, 1, verdict, sizeof verdict);
  feed_host(0, 0, ready, sizeof ready);
  TEST_ASSERT_TRUE(node.connected());
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_STATUS_OK, r2s_actions_status(0));
}

/* covers: ACT-1, ACT-7 */
void test_action_registration_binds_the_table_and_walks(void)
{
    /* DECLARE index 0, the Fibonacci hash, type and action names. */
  const uint8_t query[] = {0x01u, 0x00u};
  const uint8_t declare[] = {0x02u, 0x00u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u,
    0x1au,
    't', 'e', 's', 't', '_', 'm', 's', 'g', 's', '/', 'a',
    'c', 't', 'i', 'o', 'n', '/', 'F', 'i', 'b', 'o', 'n',
    'a', 'c', 'c', 'i', 0x04u, 's', 'p', 'i', 'n'};
  r2s::Node node("base_controller");
  r2s::action::Server<Spin> fib("spin", on_goal, on_cancel, on_accepted);
  r2s_action_t entry;

  reset_calls();
  TEST_ASSERT_FALSE(r2s::action::registration_error());
  TEST_ASSERT_EQUAL_UINT8(1, r2s_actions_count());
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_copy(0, &entry));
  TEST_ASSERT_EQUAL_STRING("test_msgs/action/Fibonacci", entry.type_name);
  TEST_ASSERT_EQUAL_STRING("spin", entry.action_name);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(s_action_hash, entry.type_hash, 8u);
  node.spin_some();
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  fake_port_tx_clear();
  feed_host(8, 0, query, sizeof query);
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 0, declare, sizeof declare);
}

/* covers: ACT-3, ACT-8 */
void test_action_goal_accepted_runs_the_callbacks(void)
{
  uint8_t goal_payload[R2S_MAX_FRAME];
  uint8_t goal[4 + R2S_MAX_FRAME];
  const uint8_t ack[] = {0x08u, 0x00u};
  const uint8_t accepted[] = {0x05u, 0x00u, 0x00u, 0x00u};
  uint16_t goal_len;
  uint16_t i;
  r2s::Node node("base_controller");
  r2s::action::Server<Spin> fib("spin", on_goal, on_cancel, on_accepted);

  reset_calls();
  connect_resolved(node);
  fake_port_tx_clear();
  goal_len = pack_float(5.0f, goal_payload);
  goal[0] = 0x04u;
  goal[1] = 0x00u;
  goal[2] = 0x00u;
  goal[3] = 0x00u;
  for (i = 0u; i < goal_len; i++) {
    goal[4u + i] = goal_payload[i];
  }
  feed_host(8, 0, goal, (uint16_t)(4u + goal_len));
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(0, 6, 0, ack, sizeof ack);
  assert_tx(1, 8, 1, accepted, sizeof accepted);
  TEST_ASSERT_EQUAL_UINT8(1, s_goal_calls);
  TEST_ASSERT_TRUE(s_goal_seen.data == 5.0f);
  TEST_ASSERT_EQUAL_UINT8(1, s_accepted_calls);
  TEST_ASSERT_NOT_NULL(s_handle);
  TEST_ASSERT_TRUE(s_handle->get_goal().data == 5.0f);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
}

/* covers: ACT-4, ACT-8 */
void test_action_goal_rejected_leaves_idle(void)
{
  uint8_t goal_payload[R2S_MAX_FRAME];
  uint8_t goal[4 + R2S_MAX_FRAME];
  const uint8_t rejected[] = {0x05u, 0x00u, 0x00u, 0x01u};
  uint16_t goal_len;
  uint16_t i;
  r2s::Node node("base_controller");
  r2s::action::Server<Spin> fib("spin", on_goal, on_cancel, on_accepted);

  reset_calls();
  s_goal_decision = r2s::action::GoalResponse::REJECT;
  connect_resolved(node);
  fake_port_tx_clear();
  goal_len = pack_float(5.0f, goal_payload);
  goal[0] = 0x04u;
  goal[1] = 0x00u;
  goal[2] = 0x00u;
  goal[3] = 0x00u;
  for (i = 0u; i < goal_len; i++) {
    goal[4u + i] = goal_payload[i];
  }
  feed_host(8, 0, goal, (uint16_t)(4u + goal_len));
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 1, rejected, sizeof rejected);
  TEST_ASSERT_EQUAL_UINT8(0, s_accepted_calls);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
}

/* covers: ACT-4, ACT-8 */
void test_action_second_goal_is_busy(void)
{
  uint8_t payload[R2S_MAX_FRAME];
  uint8_t frame[4 + R2S_MAX_FRAME];
  const uint8_t host_ack[] = {0x08u, 0x00u};
  const uint8_t busy[] = {0x05u, 0x01u, 0x01u, 0x02u};
  uint16_t len;
  uint16_t i;
  r2s::Node node("base_controller");
  r2s::action::Server<Spin> fib("spin", on_goal, on_cancel, on_accepted);

  reset_calls();
  connect_resolved(node);
  fake_port_tx_clear();
  len = pack_float(5.0f, payload);
  frame[0] = 0x04u;
  frame[1] = 0x00u;
  frame[2] = 0x00u;
  frame[3] = 0x00u;
  for (i = 0u; i < len; i++) {
    frame[4u + i] = payload[i];
  }
  feed_host(8, 0, frame, (uint16_t)(4u + len));
  feed_host(6, 0, host_ack, sizeof host_ack);
  len = pack_float(6.0f, payload);
  frame[1] = 0x01u;
  frame[2] = 0x01u;
  for (i = 0u; i < len; i++) {
    frame[4u + i] = payload[i];
  }
  feed_host(8, 1, frame, (uint16_t)(4u + len));
  TEST_ASSERT_EQUAL_UINT8(1, s_goal_calls);
  TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
  assert_tx(3, 8, 2, busy, sizeof busy);
  TEST_ASSERT_TRUE(s_handle->get_goal().data == 5.0f);
}

/* covers: ACT-5, ACT-8 */
void test_action_cancel_accepted_then_canceled(void)
{
  uint8_t payload[R2S_MAX_FRAME];
  uint8_t frame[4 + R2S_MAX_FRAME];
  const uint8_t goal_ack[] = {0x08u, 0x00u};
  const uint8_t cancel[] = {0x06u, 0x01u, 0x00u};
  const uint8_t cancel_ack[] = {0x08u, 0x01u};
  const uint8_t cancel_accepted[] = {0x07u, 0x01u, 0x00u, 0x00u};
  uint8_t result_payload[R2S_MAX_FRAME];
  uint8_t want[4 + R2S_MAX_FRAME];
  uint16_t len;
  uint16_t i;
  r2s::Node node("base_controller");
  r2s::action::Server<Spin> fib("spin", on_goal, on_cancel, on_accepted);
  Float32 result;

  reset_calls();
  connect_resolved(node);
  fake_port_tx_clear();
  len = pack_float(5.0f, payload);
  frame[0] = 0x04u;
  frame[1] = 0x00u;
  frame[2] = 0x00u;
  frame[3] = 0x00u;
  for (i = 0u; i < len; i++) {
    frame[4u + i] = payload[i];
  }
  feed_host(8, 0, frame, (uint16_t)(4u + len));
  feed_host(6, 0, goal_ack, sizeof goal_ack);
  feed_host(8, 1, cancel, sizeof cancel);
  TEST_ASSERT_EQUAL_UINT8(1, s_cancel_calls);
  TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
  assert_tx(3, 8, 2, cancel_accepted, sizeof cancel_accepted);
  TEST_ASSERT_TRUE(s_handle->is_canceling());
  feed_host(6, 1, cancel_ack, sizeof cancel_ack);
  fake_port_tx_clear();
  result.data = 3.0f;
  len = pack_float(3.0f, result_payload);
  want[0] = 0x09u;
  want[1] = 0x02u;
  want[2] = 0x00u;
  want[3] = 0x05u;
  for (i = 0u; i < len; i++) {
    want[4u + i] = result_payload[i];
  }
  TEST_ASSERT_EQUAL_INT8(R2S_OK, s_handle->canceled(result));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 3, want, (uint16_t)(4u + len));
}

/* covers: ACT-3, ACT-8 */
void test_action_feedback_and_succeed(void)
{
  uint8_t payload[R2S_MAX_FRAME];
  uint8_t frame[4 + R2S_MAX_FRAME];
  const uint8_t goal_ack[] = {0x08u, 0x00u};
  uint8_t feedback_payload[R2S_MAX_FRAME];
  uint8_t feedback_want[3 + R2S_MAX_FRAME];
  uint8_t result_payload[R2S_MAX_FRAME];
  uint8_t result_want[4 + R2S_MAX_FRAME];
  uint16_t len;
  uint16_t i;
  r2s::Node node("base_controller");
  r2s::action::Server<Spin> fib("spin", on_goal, on_cancel, on_accepted);
  Float32 feedback;
  Float32 result;

  reset_calls();
  connect_resolved(node);
  fake_port_tx_clear();
  len = pack_float(5.0f, payload);
  frame[0] = 0x04u;
  frame[1] = 0x00u;
  frame[2] = 0x00u;
  frame[3] = 0x00u;
  for (i = 0u; i < len; i++) {
    frame[4u + i] = payload[i];
  }
  feed_host(8, 0, frame, (uint16_t)(4u + len));
  feed_host(6, 0, goal_ack, sizeof goal_ack);
  fake_port_tx_clear();
  feedback.data = 2.0f;
  len = pack_float(2.0f, feedback_payload);
  feedback_want[0] = 0x08u;
  feedback_want[1] = 0x00u;
  feedback_want[2] = 0x00u;
  for (i = 0u; i < len; i++) {
    feedback_want[3u + i] = feedback_payload[i];
  }
  TEST_ASSERT_EQUAL_INT8(R2S_OK, s_handle->publish_feedback(feedback));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 2, feedback_want, (uint16_t)(3u + len));
  result.data = 15.0f;
  len = pack_float(15.0f, result_payload);
  result_want[0] = 0x09u;
  result_want[1] = 0x01u;
  result_want[2] = 0x00u;
  result_want[3] = 0x04u;
  for (i = 0u; i < len; i++) {
    result_want[4u + i] = result_payload[i];
  }
  TEST_ASSERT_EQUAL_INT8(R2S_OK, s_handle->succeed(result));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 3, result_want, (uint16_t)(4u + len));
}

/* covers: ACT-6, ACT-8 */
void test_action_abort_reports_aborted(void)
{
  uint8_t payload[R2S_MAX_FRAME];
  uint8_t frame[4 + R2S_MAX_FRAME];
  const uint8_t goal_ack[] = {0x08u, 0x00u};
  uint8_t result_payload[R2S_MAX_FRAME];
  uint8_t want[4 + R2S_MAX_FRAME];
  uint16_t len;
  uint16_t i;
  r2s::Node node("base_controller");
  r2s::action::Server<Spin> fib("spin", on_goal, on_cancel, on_accepted);
  Float32 result;

  reset_calls();
  connect_resolved(node);
  fake_port_tx_clear();
  len = pack_float(5.0f, payload);
  frame[0] = 0x04u;
  frame[1] = 0x00u;
  frame[2] = 0x00u;
  frame[3] = 0x00u;
  for (i = 0u; i < len; i++) {
    frame[4u + i] = payload[i];
  }
  feed_host(8, 0, frame, (uint16_t)(4u + len));
  feed_host(6, 0, goal_ack, sizeof goal_ack);
  fake_port_tx_clear();
  result.data = 0.0f;
  len = pack_float(0.0f, result_payload);
  want[0] = 0x09u;
  want[1] = 0x01u;
  want[2] = 0x00u;
  want[3] = 0x06u;
  for (i = 0u; i < len; i++) {
    want[4u + i] = result_payload[i];
  }
  TEST_ASSERT_EQUAL_INT8(R2S_OK, s_handle->abort(result));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 2, want, (uint16_t)(4u + len));
}

/* covers: ACT-8, ACT-9 */
void test_action_handle_calls_fail_without_a_running_goal(void)
{
  uint8_t payload[R2S_MAX_FRAME];
  uint8_t frame[4 + R2S_MAX_FRAME];
  const uint8_t goal_ack[] = {0x08u, 0x00u};
  const uint8_t result_ack[] = {0x08u, 0x01u};
  uint16_t len;
  uint16_t i;
  r2s::Node node("base_controller");
  r2s::action::Server<Spin> fib("spin", on_goal, on_cancel, on_accepted);
  Float32 message;

  reset_calls();
  connect_resolved(node);
  fake_port_tx_clear();
  len = pack_float(5.0f, payload);
  frame[0] = 0x04u;
  frame[1] = 0x00u;
  frame[2] = 0x00u;
  frame[3] = 0x00u;
  for (i = 0u; i < len; i++) {
    frame[4u + i] = payload[i];
  }
  feed_host(8, 0, frame, (uint16_t)(4u + len));
  feed_host(6, 0, goal_ack, sizeof goal_ack);
  message.data = 1.0f;
  TEST_ASSERT_EQUAL_INT8(R2S_OK, s_handle->succeed(message));
  feed_host(6, 1, result_ack, sizeof result_ack);
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, s_handle->publish_feedback(message));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, s_handle->succeed(message));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-8 */
void test_action_static_rows_declare_and_run(void)
{
#define TEST_ACTIONS(X) X(Spin, static_fib, "spin", on_goal, on_cancel, on_accepted)
  r2s::Node node("base_controller");

  reset_calls();
  TEST_ACTIONS(R2S_ACTION_X_SERVER);
  connect_resolved(node);
  TEST_ASSERT_FALSE(r2s::action::registration_error());
  TEST_ASSERT_EQUAL_UINT8(1, r2s_actions_count());
  TEST_ASSERT_EQUAL_UINT8(0, static_fib.index());
}
