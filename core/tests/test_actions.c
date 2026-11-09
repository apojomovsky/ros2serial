/* Actions on the MCU: the announced table, the walk answers, the one-goal
 * state machine and the reliable exchange (spec/protocol.md, section 7.9).
 * The upcalls below stand in for the C++ layer: each test names the
 * decision the layer would return. */

#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_actions.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

static uint8_t wire[WIRE_MAX];

static const r2s_topic_t one_topic[] = {
  {0x10u, 0x00u, {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u}, "std_msgs/msg/Float32",
    "float"},
};

/* The announced Fibonacci action, with the hash the frame vectors carry. */
static const r2s_action_t one_action[] = {
  {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, "test_msgs/action/Fibonacci", "spin"},
};

/* What the standing upcalls answer with, and what they saw. configure()
 * resets every counter, so each test starts uncalled. */
static uint8_t g_goal_decision;
static uint8_t g_goal_calls;
static uint8_t g_goal_index;
static uint8_t g_goal_seq;
static uint16_t g_goal_len;
static uint8_t g_cancel_decision;
static uint8_t g_cancel_calls;
static uint8_t g_cancel_index;
static uint8_t g_cancel_seq;

void r2s_action_on_goal(uint8_t index, uint8_t goal_seq, const uint8_t *payload, uint16_t len)
{
  (void)payload;
  g_goal_calls++;
  g_goal_index = index;
  g_goal_seq = goal_seq;
  g_goal_len = len;
  (void)r2s_actions_answer_goal(index, goal_seq, g_goal_decision);
}

void r2s_action_on_cancel(uint8_t index, uint8_t goal_seq)
{
  g_cancel_calls++;
  g_cancel_index = index;
  g_cancel_seq = goal_seq;
  (void)r2s_actions_answer_cancel(index, goal_seq, g_cancel_decision);
}

static void configure(void)
{
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_configure(one_action, 1));
  TEST_ASSERT_EQUAL_UINT8(1, r2s_actions_count());
  g_goal_decision = R2S_ACT_ACCEPTED;
  g_goal_calls = 0u;
  g_goal_index = 0xFFu;
  g_goal_seq = 0xFFu;
  g_goal_len = 0xFFFFu;
  g_cancel_decision = R2S_ACT_CANCEL_ACCEPTED;
  g_cancel_calls = 0u;
  g_cancel_index = 0xFFu;
  g_cancel_seq = 0xFFu;
  r2s_init();
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
  uint16_t wire_len = r2s_test_frame(wire, topic_id, seq, payload, len);

  feed_slow(wire, wire_len);
}

/* Checks the index-th captured frame against the wire form of its topic, seq
 * and payload, the same comparison the frames conformance test makes. */
static void assert_tx(
  uint16_t index, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
  uint16_t payload_len)
{
  static uint8_t expected[WIRE_MAX];
  char label[64];
  uint16_t encoded_len;
  const uint8_t *encoded;
  uint16_t expected_len = r2s_test_frame(expected, topic_id, seq, payload, payload_len);

  snprintf(label, sizeof label, "tx frame %u", index);
  encoded = fake_port_tx_frame(index, &encoded_len);
  TEST_ASSERT_NOT_NULL_MESSAGE(encoded, label);
    /* The captured frame stops at its delimiter, which the built wire keeps. */
  R2S_ASSERT_BYTES(expected, (uint16_t)(expected_len - 1u), encoded, encoded_len, label);
}

/* Moves to CONNECTED the way the handshake does: READY with a nonzero
 * token, answered in ANNOUNCING. */
static void connect(void)
{
  const uint8_t ready[] = {0x06u, 0x01u, 0x00u, 0x00u, 0x00u};

  feed_host(0, 0, ready, sizeof ready);
  TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
}

/* Answers the walk for the bound action, so later tests start resolved. */
static void resolve_walk(void)
{
  const uint8_t declare_answer[] = {0x03u, 0x00u, 0x00u, 0x00u};

  feed_host(8, 0, (const uint8_t[]) {0x01u, 0x00u}, 2);
  feed_host(8, 1, declare_answer, sizeof declare_answer);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_STATUS_OK, r2s_actions_status(0));
}

/* covers: ACT-7 */
void test_actions_count_matches_configured_table(void)
{
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_count());
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_configure(one_action, 1));
  TEST_ASSERT_EQUAL_UINT8(1, r2s_actions_count());
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_configure(NULL, 0));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_count());
}

/* covers: ACT-7 */
void test_actions_get_copy_and_declare_len(void)
{
  const r2s_action_t *got;
  r2s_action_t out;
  uint8_t hash[8] = {0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u};

  configure();
  got = r2s_actions_get(0u);
  if (got == NULL) {
    TEST_FAIL_MESSAGE("bound table entry missing");
    return;
  }
  TEST_ASSERT_EQUAL_STRING("test_msgs/action/Fibonacci", got->type_name);
  TEST_ASSERT_EQUAL_STRING("spin", got->action_name);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(hash, got->type_hash, 8u);
  TEST_ASSERT_NULL(r2s_actions_get(1u));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_copy(0u, &out));
  TEST_ASSERT_EQUAL_STRING("spin", out.action_name);
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_copy(1u, &out));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_copy(0u, NULL));
    /* Opcode, index, hash, the 26-byte type name and the 4-byte action name. */
  TEST_ASSERT_EQUAL_UINT16(1u + 1u + 8u + 1u + 26u + 1u + 4u, r2s_actions_declare_len(0u));
  TEST_ASSERT_EQUAL_UINT16(0u, r2s_actions_declare_len(1u));
}

/* covers: ACT-7 */
void test_actions_configure_refuses_a_table_it_cannot_announce(void)
{
  static r2s_action_t null_type[] = {
    {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, NULL, "spin"},
  };
  static r2s_action_t null_name[] = {
    {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, "test_msgs/action/Fibonacci", NULL},
  };
  static char long_name[600];
  static r2s_action_t long_table[] = {
    {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, "test_msgs/action/Fibonacci", ""},
  };
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_configure(NULL, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_configure(null_type, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_configure(null_name, 1));
  memset(long_name, 'n', sizeof long_name - 1u);
  long_name[sizeof long_name - 1u] = '\0';
  long_table[0].action_name = long_name;
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_configure(long_table, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_configure(one_action, (uint8_t)(R2S_ACTION_MAX_COUNT + 1u)));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_count());
}

/* covers: ACT-1, ACT-7 */
void test_actions_query_known_index_answers_declare(void)
{
  const uint8_t query[] = {0x01u, 0x00u};
    /* DECLARE index 0, the Fibonacci hash, type and action names. */
  const uint8_t declare[] = {0x02u, 0x00u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u,
    0x1au,
    't', 'e', 's', 't', '_', 'm', 's', 'g', 's', '/', 'a',
    'c', 't', 'i', 'o', 'n', '/', 'F', 'i', 'b', 'o', 'n',
    'a', 'c', 'c', 'i', 0x04u, 's', 'p', 'i', 'n'};

  configure();
  feed_host(8, 0, query, sizeof query);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 0, declare, sizeof declare);
}

/* covers: ACT-7 */
void test_actions_query_for_unknown_index_answers_status_1_plain(void)
{
  const uint8_t query[] = {0x01u, 0x09u};
    /* A plain reply under the request retry carries rel_seq 0. */
  const uint8_t status[] = {0x03u, 0x00u, 0x09u, 0x01u};

  configure();
  feed_host(8, 0, query, sizeof query);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 0, status, sizeof status);
}

/* covers: ACT-7 */
void test_actions_status_records_the_host_verdict(void)
{
  const uint8_t verdict[] = {0x03u, 0x00u, 0x00u, 0x00u};

  configure();
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_WALK_UNKNOWN, r2s_actions_status(0));
  feed_host(8, 0, verdict, sizeof verdict);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_STATUS_OK, r2s_actions_status(0));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_WALK_UNKNOWN, r2s_actions_status(1));
}

/* covers: ACT-1, ACT-7, SYS-5 */
void test_actions_bad_walk_frames_are_refused(void)
{
  const uint8_t short_query[] = {0x01u};
  const uint8_t bad_status[] = {0x03u, 0x00u, 0x00u, 0x09u};
  const uint8_t status_past[] = {0x03u, 0x00u, 0x05u, 0x00u};

  configure();
  feed_host(8, 0, short_query, sizeof short_query);
  feed_host(8, 1, bad_status, sizeof bad_status);
  feed_host(8, 2, status_past, sizeof status_past);
    /* HELLO plus three refused frames: nothing else goes out. */
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_WALK_UNKNOWN, r2s_actions_status(0));
}

/* covers: ACT-9, SES-3 */
void test_actions_goal_in_announcing_is_answered_with_hello(void)
{
    /* GOAL rel 0, goal 0, action 0, order 5. */
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};

  configure();
  feed_host(8, 0, goal, sizeof goal);
  TEST_ASSERT_EQUAL_UINT8(0, g_goal_calls);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
}

/* covers: ACT-3, ACT-8 */
void test_actions_goal_accepted_answers_and_runs(void)
{
    /* GOAL rel 0, goal 0, action 0, order 5. */
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t ack[] = {0x08u, 0x00u};
  const uint8_t accepted[] = {0x05u, 0x00u, 0x00u, 0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, goal, sizeof goal);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(0, 6, 0, ack, sizeof ack);
  assert_tx(1, 8, 1, accepted, sizeof accepted);
  TEST_ASSERT_EQUAL_UINT8(1, g_goal_calls);
  TEST_ASSERT_EQUAL_UINT8(0, g_goal_index);
  TEST_ASSERT_EQUAL_UINT8(0, g_goal_seq);
  TEST_ASSERT_EQUAL_UINT16(4, g_goal_len);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_goal_seq(0));
}

/* covers: ACT-4, ACT-8 */
void test_actions_goal_rejected_leaves_idle(void)
{
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t rejected[] = {0x05u, 0x00u, 0x00u, 0x01u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  g_goal_decision = R2S_ACT_REJECTED;
  feed_host(8, 0, goal, sizeof goal);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 1, rejected, sizeof rejected);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
}

/* covers: ACT-4 */
void test_actions_goal_for_unknown_index_answers_unknown_action(void)
{
    /* GOAL rel 0, goal 0, action 5 past the table. */
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x05u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t ack[] = {0x08u, 0x00u};
  const uint8_t unknown[] = {0x05u, 0x00u, 0x00u, 0x03u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, goal, sizeof goal);
  TEST_ASSERT_EQUAL_UINT8(0, g_goal_calls);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(0, 6, 0, ack, sizeof ack);
  assert_tx(1, 8, 1, unknown, sizeof unknown);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
}

/* covers: ACT-4, ACT-7 */
void test_actions_goal_with_unresolved_walk_answers_unknown_action(void)
{
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t unknown[] = {0x05u, 0x00u, 0x00u, 0x03u};

  configure();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, goal, sizeof goal);
  TEST_ASSERT_EQUAL_UINT8(0, g_goal_calls);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 0, unknown, sizeof unknown);
}

/* covers: ACT-4, ACT-8 */
void test_actions_second_goal_is_busy_and_leaves_the_active_goal(void)
{
    /* First goal 0, order 5; second goal 1, order 6, new rel seq. */
  const uint8_t first[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t second[] = {0x04u, 0x01u, 0x01u, 0x00u, 0x06u, 0x00u, 0x00u, 0x00u};
  const uint8_t second_ack[] = {0x08u, 0x01u};
  const uint8_t busy[] = {0x05u, 0x01u, 0x01u, 0x02u};
  const uint8_t host_ack[] = {0x08u, 0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, first, sizeof first);
  feed_host(6, 0, host_ack, sizeof host_ack);
  feed_host(8, 1, second, sizeof second);
  TEST_ASSERT_EQUAL_UINT8(1, g_goal_calls);
  TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
  assert_tx(2, 6, 1, second_ack, sizeof second_ack);
  assert_tx(3, 8, 2, busy, sizeof busy);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_goal_seq(0));
}

/* covers: REL-3 */
void test_actions_duplicate_goal_is_acked_not_applied(void)
{
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t resend[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x06u, 0x00u, 0x00u, 0x00u};
  const uint8_t ack[] = {0x08u, 0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, goal, sizeof goal);
  feed_host(8, 1, resend, sizeof resend);
  TEST_ASSERT_EQUAL_UINT8(1, g_goal_calls);
  TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
  assert_tx(2, 6, 1, ack, sizeof ack);
}

/* covers: ACT-1, SYS-5 */
void test_actions_bad_goal_and_cancel_frames_are_refused(void)
{
  const uint8_t short_goal[] = {0x04u, 0x00u, 0x00u};
  const uint8_t bad_op[] = {0x0Au, 0x00u, 0x00u, 0x00u};
  const uint8_t short_cancel[] = {0x06u, 0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, short_goal, sizeof short_goal);
  feed_host(8, 1, bad_op, sizeof bad_op);
  feed_host(8, 2, short_cancel, sizeof short_cancel);
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
  TEST_ASSERT_EQUAL_UINT8(0, g_goal_calls);
  TEST_ASSERT_EQUAL_UINT8(0, g_cancel_calls);
}

/* covers: ACT-7 */
void test_actions_query_in_connected_is_refused(void)
{
  const uint8_t query[] = {0x01u, 0x00u};

  configure();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, query, sizeof query);
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* Starts goal 0, order 5, accepted, with the walk resolved. */
static void start_goal(void)
{
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, goal, sizeof goal);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
}

/* covers: ACT-5, ACT-8 */
void test_actions_cancel_accepted_moves_to_canceling(void)
{
    /* CANCEL rel 1, goal 0. */
  const uint8_t cancel[] = {0x06u, 0x01u, 0x00u};
  const uint8_t ack[] = {0x08u, 0x01u};
  const uint8_t accepted[] = {0x07u, 0x01u, 0x00u, 0x00u};
  const uint8_t goal_ack[] = {0x08u, 0x00u};

  start_goal();
  feed_host(6, 0, goal_ack, sizeof goal_ack);
  fake_port_tx_clear();
  feed_host(8, 1, cancel, sizeof cancel);
  TEST_ASSERT_EQUAL_UINT8(1, g_cancel_calls);
  TEST_ASSERT_EQUAL_UINT8(0, g_cancel_index);
  TEST_ASSERT_EQUAL_UINT8(0, g_cancel_seq);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(0, 6, 1, ack, sizeof ack);
  assert_tx(1, 8, 2, accepted, sizeof accepted);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_CANCELING, r2s_actions_state(0));
}

/* covers: ACT-5 */
void test_actions_cancel_for_unknown_goal_answers_unknown(void)
{
    /* CANCEL rel 0, goal 7 with no active goal. */
  const uint8_t cancel[] = {0x06u, 0x00u, 0x07u};
  const uint8_t unknown[] = {0x07u, 0x00u, 0x07u, 0x02u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, cancel, sizeof cancel);
  TEST_ASSERT_EQUAL_UINT8(0, g_cancel_calls);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 1, unknown, sizeof unknown);
}

/* covers: ACT-5, ACT-8 */
void test_actions_cancel_rejected_stays_executing(void)
{
  const uint8_t cancel[] = {0x06u, 0x01u, 0x00u};
  const uint8_t rejected[] = {0x07u, 0x01u, 0x00u, 0x01u};
  const uint8_t goal_ack[] = {0x08u, 0x00u};

  start_goal();
  feed_host(6, 0, goal_ack, sizeof goal_ack);
  fake_port_tx_clear();
  g_cancel_decision = R2S_ACT_CANCEL_REJECTED;
  feed_host(8, 1, cancel, sizeof cancel);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 2, rejected, sizeof rejected);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
}

/* covers: ACT-5 */
void test_actions_duplicate_cancel_is_acked_not_applied(void)
{
  const uint8_t cancel[] = {0x06u, 0x01u, 0x00u};
  const uint8_t ack[] = {0x08u, 0x01u};
  const uint8_t goal_ack[] = {0x08u, 0x00u};

  start_goal();
  feed_host(6, 0, goal_ack, sizeof goal_ack);
  fake_port_tx_clear();
  feed_host(8, 1, cancel, sizeof cancel);
  feed_host(8, 2, cancel, sizeof cancel);
  TEST_ASSERT_EQUAL_UINT8(1, g_cancel_calls);
  TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
  assert_tx(2, 6, 2, ack, sizeof ack);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_CANCELING, r2s_actions_state(0));
}

/* covers: ACT-3, ACT-8 */
void test_actions_feedback_sends_best_effort_while_executing(void)
{
    /* Partial sequence [0, 1, 2] as packed Feedback bytes. */
  const uint8_t feedback[] = {0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u,
    0x00u, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u};
  const uint8_t want[] = {0x08u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
    0x00u, 0x00u, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u};
  const uint8_t host_ack[] = {0x08u, 0x00u};

  start_goal();
  feed_host(6, 0, host_ack, sizeof host_ack);
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_feedback(0u, 0u, feedback, sizeof feedback));
    /* Best effort: one frame, no rel_seq, no ack chatter. */
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 2, want, sizeof want);
}

/* covers: ACT-8 */
void test_actions_feedback_while_idle_sends_nothing(void)
{
  const uint8_t feedback[] = {0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_feedback(0u, 0u, feedback, sizeof feedback));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_feedback(0u, 7u, feedback, sizeof feedback));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_feedback(5u, 0u, feedback, sizeof feedback));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-9 */
void test_actions_feedback_while_announcing_fails_not_connected(void)
{
  const uint8_t feedback[] = {0x00u};
    /* code 5, actions channel, one occurrence, no field. */
  const uint8_t report[] = {0x05u, 0x08u, 0x01u, 0x00u, 0xFFu};

  configure();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED,
    r2s_actions_feedback(0u, 0u, feedback, sizeof feedback));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 5, 0, report, sizeof report);
}

/* covers: ACT-3, ACT-6, ACT-8 */
void test_actions_result_succeeds_and_returns_to_idle(void)
{
    /* Sequence [0, 1, 2, 3] as packed Result bytes, succeeded. */
  const uint8_t result[] = {0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u,
    0x02u, 0x00u, 0x00u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u};
  const uint8_t want[] = {0x09u, 0x01u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u,
    0x00u, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u};
  const uint8_t host_ack[] = {0x08u, 0x00u};
  const uint8_t result_ack[] = {0x08u, 0x01u};

  start_goal();
  feed_host(6, 0, host_ack, sizeof host_ack);
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK,
    r2s_actions_result(0u, 0u, R2S_ACT_SUCCEEDED, result, sizeof result));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 2, want, sizeof want);
  feed_host(6, 1, result_ack, sizeof result_ack);
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}

/* covers: ACT-6, ACT-8 */
void test_actions_canceled_result_after_accepted_cancel(void)
{
  const uint8_t cancel[] = {0x06u, 0x01u, 0x00u};
  const uint8_t goal_ack[] = {0x08u, 0x00u};
  const uint8_t host_ack_cancel[] = {0x08u, 0x01u};
  const uint8_t canceled[] = {0x09u, 0x02u, 0x00u, 0x05u};

  start_goal();
  feed_host(6, 0, goal_ack, sizeof goal_ack);
  feed_host(8, 1, cancel, sizeof cancel);
  feed_host(6, 1, host_ack_cancel, sizeof host_ack_cancel);
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_result(0u, 0u, R2S_ACT_CANCELED, NULL, 0));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 3, canceled, sizeof canceled);
}

/* covers: ACT-8 */
void test_actions_result_without_active_goal_sends_nothing(void)
{
  const uint8_t result[] = {0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_result(0u, 0u, R2S_ACT_SUCCEEDED, result, sizeof result));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_result(0u, 7u, R2S_ACT_SUCCEEDED, result, sizeof result));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_result(5u, 0u, R2S_ACT_SUCCEEDED, result, sizeof result));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-6 */
void test_actions_result_with_bad_outcome_sends_nothing(void)
{
  const uint8_t result[] = {0x00u};

  start_goal();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_result(0u, 0u, 3u, result, sizeof result));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_result(0u, 0u, 7u, result, sizeof result));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-4, ACT-5 */
void test_actions_answers_refuse_bad_decisions(void)
{
  start_goal();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_answer_goal(0u, 0u, 4u));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_answer_cancel(0u, 0u, 3u));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_answer_goal(5u, 0u, R2S_ACT_REJECTED));
}

/* covers: REL-2, REL-4 */
void test_actions_goal_resp_retransmits_then_gives_up_counted(void)
{
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t accepted[] = {0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t empty[1] = {0u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, goal, sizeof goal);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    /* Three retries, each with the next frame seq and the same rel seq. The
     * walk fed two frames past the params test's one, so the heartbeat
     * counts start at four received. */
  for (uint16_t retry = 1u; retry <= 3u; retry++) {
    uint8_t want[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

    fake_port_advance(500);
    feed_host(4, (uint8_t)(retry - 1u), empty, 0);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(2u + 2u * retry), fake_port_tx_frame_count());
    assert_tx((uint16_t)(2u * retry), 8, (uint8_t)(retry + 1u), accepted, sizeof accepted);
    want[0] = (uint8_t)(retry + 3u);
    assert_tx((uint16_t)(2u * retry + 1u), 4, (uint8_t)(retry - 1u), want, sizeof want);
  }
    /* Spent: no fourth response, and the drop counted with the heartbeat. */
  {
    uint8_t want[8] = {7u, 0u, 0u, 0u, 0u, 0u, 1u, 0u};

    fake_port_advance(500);
    feed_host(4, 3, empty, 0);
    TEST_ASSERT_EQUAL_UINT16(9, fake_port_tx_frame_count());
    assert_tx(8, 4, 3, want, sizeof want);
  }
}

/* covers: REL-1, REL-2 */
void test_actions_host_ack_stops_the_retry(void)
{
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t ack[] = {0x08u, 0x00u};
  const uint8_t empty[1] = {0u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, goal, sizeof goal);
  feed_host(6, 0, ack, sizeof ack);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  for (uint8_t step = 0u; step < 4u; step++) {
    fake_port_advance(500);
    feed_host(4, step, empty, 0);
  }
  TEST_ASSERT_EQUAL_UINT16(6, fake_port_tx_frame_count());
}

/* covers: REL-2 */
void test_actions_full_slots_hold_the_goal_for_retry(void)
{
    /* GOAL rel 0, goal 0, order 5; RESULT rel 1, succeeded. */
  const uint8_t first[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t result[] = {0x00u};
    /* GOAL rel 1, goal 1, order 6. */
  const uint8_t second[] = {0x04u, 0x01u, 0x01u, 0x00u, 0x06u, 0x00u, 0x00u, 0x00u};
    /* A TX_BUSY report names the actions channel and the held action. */
  const uint8_t busy[] = {0x06u, 0x08u, 0x01u, 0x00u, 0x00u};
  const uint8_t ack_resp[] = {0x08u, 0x00u};
  const uint8_t ack_result[] = {0x08u, 0x01u};
  const uint8_t accepted[] = {0x05u, 0x02u, 0x01u, 0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, first, sizeof first);
  TEST_ASSERT_EQUAL_INT8(R2S_OK,
    r2s_actions_result(0u, 0u, R2S_ACT_SUCCEEDED, result, sizeof result));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
    /* A second past the busy window: the retry timer resends the
     * outstanding response and the heartbeat goes out with it. */
  fake_port_advance(1000);
  fake_port_tx_clear();
  {
    const uint8_t retried[] = {0x05u, 0x00u, 0x00u, 0x00u};
    const uint8_t heartbeat[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

    feed_host(8, 1, second, sizeof second);
    TEST_ASSERT_EQUAL_UINT8(1, g_goal_calls);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    assert_tx(0, 8, 2, retried, sizeof retried);
    assert_tx(1, 4, 0, heartbeat, sizeof heartbeat);
    assert_tx(2, 5, 0, busy, sizeof busy);
      /* A held frame is not a refused one: the hold reports TX_BUSY but
       * drops nothing. */
    TEST_ASSERT_EQUAL_UINT16(0, r2s_framer_counters()->dropped_frames);
  }
    /* The acks free both slots; the retried goal then runs at once. */
  feed_host(6, 0, ack_resp, sizeof ack_resp);
  feed_host(6, 1, ack_result, sizeof ack_result);
  fake_port_tx_clear();
  feed_host(8, 1, second, sizeof second);
  TEST_ASSERT_EQUAL_UINT8(2, g_goal_calls);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 4, accepted, sizeof accepted);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT8(1, r2s_actions_goal_seq(0));
}

/* covers: ACT-8 */
void test_actions_feedback_and_result_refuse_a_foreign_goal_seq(void)
{
  const uint8_t feedback[] = {0x00u};
  const uint8_t result[] = {0x00u};

  start_goal();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_feedback(0u, 7u, feedback, sizeof feedback));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_result(0u, 7u, R2S_ACT_SUCCEEDED, result, sizeof result));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-4, ACT-5 */
void test_actions_answers_refuse_a_goal_the_state_no_longer_holds(void)
{
  const uint8_t result[] = {0x00u};

  start_goal();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_answer_goal(0u, 0u, R2S_ACT_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
    r2s_actions_answer_cancel(0u, 7u, R2S_ACT_CANCEL_ACCEPTED));
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  TEST_ASSERT_EQUAL_INT8(R2S_OK,
    r2s_actions_result(0u, 0u, R2S_ACT_SUCCEEDED, result, sizeof result));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT8(0u, r2s_actions_goal_seq(0));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
    r2s_actions_answer_cancel(0u, 0u, R2S_ACT_CANCEL_ACCEPTED));
}

/* covers: SES-3 */
void test_actions_empty_frame_in_announcing_is_answered_with_hello(void)
{
  const uint8_t empty[1] = {0u};

  configure();
  fake_port_tx_clear();
  feed_host(8, 0, empty, 0);
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}

/* The three-action table for the slot tests: direct answers need no walk. */
static const r2s_action_t three_actions[] = {
  {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, "test_msgs/action/Fibonacci",
    "spin0"},
  {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, "test_msgs/action/Fibonacci",
    "spin1"},
  {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, "test_msgs/action/Fibonacci",
    "spin2"},
};

static void configure_three(void)
{
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_configure(three_actions, 3));
  r2s_init();
}

/* covers: ACT-7 */
void test_actions_ram_table_copies_from_ram(void)
{
  static r2s_action_t ram_table[] = {
    {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, "test_msgs/action/Fibonacci",
      "spin"},
  };
  r2s_action_t out;

  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_configure_ram(ram_table, 1));
  TEST_ASSERT_EQUAL_UINT8(1, r2s_actions_count());
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_copy(0u, &out));
  TEST_ASSERT_EQUAL_STRING("test_msgs/action/Fibonacci", out.type_name);
  TEST_ASSERT_EQUAL_STRING("spin", out.action_name);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0u));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(1u));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_WALK_UNKNOWN, r2s_actions_status(0u));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_goal_seq(0u));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_goal_seq(1u));
}

/* covers: ACT-7 */
void test_actions_configure_refuses_a_name_past_the_varint_limit(void)
{
  static char huge[R2S_VARINT_MAX + 2u];
  static r2s_action_t huge_table[] = {
    {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, "test_msgs/action/Fibonacci",
      huge},
  };

  memset(huge, 'n', sizeof huge - 1u);
  huge[sizeof huge - 1u] = '\0';
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_configure(huge_table, 1));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_count());
}

/* covers: ACT-7 */
void test_actions_configure_refuses_a_type_past_the_varint_limit(void)
{
  static char huge[R2S_VARINT_MAX + 2u];
  static r2s_action_t huge_table[] = {
    {{0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}, huge, "spin"},
  };

  memset(huge, 't', sizeof huge - 1u);
  huge[sizeof huge - 1u] = '\0';
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_configure(huge_table, 1));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_count());
}

/* covers: ACT-9 */
void test_actions_answer_goal_refuses_when_not_connected(void)
{
  configure();
  TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED,
                           r2s_actions_answer_goal(0u, 0u, R2S_ACT_ACCEPTED));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
}

/* covers: ACT-4, REL-2 */
void test_actions_answer_goal_reports_busy_slots(void)
{
  configure_three();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_answer_goal(0u, 0u, R2S_ACT_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_answer_goal(1u, 1u, R2S_ACT_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY, r2s_actions_answer_goal(2u, 2u, R2S_ACT_ACCEPTED));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(2u));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_actions_goal_seq(2u));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}

/* covers: ACT-9 */
void test_actions_answer_cancel_refuses_when_not_connected(void)
{
  configure();
  TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED,
                           r2s_actions_answer_cancel(0u, 0u, R2S_ACT_CANCEL_ACCEPTED));
}

/* covers: ACT-8, REL-2 */
void test_actions_answer_cancel_reports_busy_slots(void)
{
  configure_three();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_answer_cancel((uint8_t)(R2S_ACTION_MAX_COUNT + 1u),
                                                       0u,
                                                       R2S_ACT_CANCEL_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_answer_cancel(2u, 0u, R2S_ACT_CANCEL_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_answer_goal(0u, 0u, R2S_ACT_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_answer_cancel(0u, 7u, R2S_ACT_CANCEL_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_answer_goal(1u, 1u, R2S_ACT_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY,
                           r2s_actions_answer_cancel(0u, 0u, R2S_ACT_CANCEL_ACCEPTED));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0u));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}

/* covers: ACT-5, REL-2 */
void test_actions_result_reports_busy_slots(void)
{
  configure_three();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_answer_goal(0u, 0u, R2S_ACT_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_answer_goal(1u, 1u, R2S_ACT_ACCEPTED));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY,
                           r2s_actions_result(0u, 0u, R2S_ACT_SUCCEEDED, NULL, 0u));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0u));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}

/* covers: ACT-5 */
void test_actions_feedback_rejects_a_null_body(void)
{
  const uint8_t empty_feedback[] = {0x08u, 0x00u, 0x00u};

  start_goal();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_feedback(0u, 0u, NULL, 1u));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_feedback(9u, 0u, NULL, 0u));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0u));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    /* A null body with no bytes is an empty feedback. */
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_feedback(0u, 0u, NULL, 0u));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 2, empty_feedback, sizeof empty_feedback);
}

/* covers: ACT-5 */
void test_actions_feedback_rejects_an_oversize_body(void)
{
  static uint8_t big[R2S_MAX_FRAME];
  const uint8_t one[] = {0x00u};

  start_goal();
  fake_port_tx_clear();
  memset(big, 0x11, sizeof big);
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_feedback(0u, 0u, big, sizeof big));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_feedback(0u, 0u, one, sizeof one));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}

/* covers: ACT-5, REL-6 */
void test_actions_feedback_fails_cleanly_without_tx_space(void)
{
  const uint8_t one[] = {0x00u};

  start_goal();
  fake_port_tx_clear();
  fake_port_set_tx_free(0u);
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY, r2s_actions_feedback(0u, 0u, one, sizeof one));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-5 */
void test_actions_feedback_refuses_without_a_goal(void)
{
  const uint8_t one[] = {0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_actions_feedback(0u, 0u, one, sizeof one));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-8 */
void test_actions_second_cancel_while_canceling_stays_canceling(void)
{
    /* CANCEL rel 1, goal 0, then CANCEL rel 2, goal 0. */
  const uint8_t cancel[] = {0x06u, 0x01u, 0x00u};
  const uint8_t again[] = {0x06u, 0x02u, 0x00u};
  const uint8_t goal_ack[] = {0x08u, 0x00u};
  const uint8_t ack[] = {0x08u, 0x02u};

  start_goal();
  feed_host(6, 0, goal_ack, sizeof goal_ack);
  fake_port_tx_clear();
  feed_host(8, 1, cancel, sizeof cancel);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_CANCELING, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT8(1, g_cancel_calls);
  fake_port_tx_clear();
  feed_host(8, 2, again, sizeof again);
  TEST_ASSERT_EQUAL_UINT8(2, g_cancel_calls);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_CANCELING, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 6, 2, ack, sizeof ack);
}

/* covers: ACT-5 */
void test_actions_result_rejects_a_null_body(void)
{
  start_goal();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_result(0u, 0u, R2S_ACT_SUCCEEDED, NULL, 1u));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0u));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-5 */
void test_actions_result_reports_canceled_and_aborted(void)
{
  const uint8_t one[] = {0x00u};
  const uint8_t ack_goal[] = {0x08u, 0x00u};
  const uint8_t canceled[] = {0x09u, 0x01u, 0x00u, 0x05u, 0x00u};
  const uint8_t aborted[] = {0x09u, 0x01u, 0x00u, 0x06u, 0x00u};

  start_goal();
  feed_host(6, 0, ack_goal, sizeof ack_goal);
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_result(0u, 0u, R2S_ACT_CANCELED, one, sizeof one));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0u));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 2, canceled, sizeof canceled);
  start_goal();
  feed_host(6, 0, ack_goal, sizeof ack_goal);
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_result(0u, 0u, R2S_ACT_ABORTED, one, sizeof one));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0u));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 2, aborted, sizeof aborted);
}

/* covers: ACT-5 */
void test_actions_result_rejects_an_oversize_body(void)
{
  static uint8_t big[R2S_MAX_FRAME];
  const uint8_t one[] = {0x00u};

  start_goal();
  fake_port_tx_clear();
  memset(big, 0x11, sizeof big);
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_actions_result(0u, 0u, R2S_ACT_SUCCEEDED, big, sizeof big));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_result(0u, 0u, R2S_ACT_SUCCEEDED, one, sizeof one));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0u));
}

/* covers: ACT-7, REL-6 */
void test_actions_declare_sends_nothing_without_tx_space(void)
{
  configure();
  fake_port_tx_clear();
  fake_port_set_tx_free(0u);
  feed_host(8, 0, (const uint8_t[]) {0x01u, 0x00u}, 2);
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-7, REL-6 */
void test_actions_status_sends_nothing_without_tx_space(void)
{
  configure();
  fake_port_tx_clear();
  fake_port_set_tx_free(0u);
  feed_host(8, 0, (const uint8_t[]) {0x01u, 0x05u}, 2);
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ACT-8, SYS-5 */
void test_actions_cancel_with_a_long_frame_is_refused(void)
{
  const uint8_t long_cancel[] = {0x06u, 0x01u, 0x00u, 0xFFu};

  start_goal();
  fake_port_tx_clear();
  feed_host(8, 1, long_cancel, sizeof long_cancel);
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
  TEST_ASSERT_EQUAL_UINT8(0, g_cancel_calls);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
}

/* covers: REL-2 */
void test_actions_full_slots_hold_the_cancel_for_retry(void)
{
    /* GOAL rel 0, goal 0, order 5. CANCEL rel 1, goal 0. */
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t cancel[] = {0x06u, 0x01u, 0x00u};
  const uint8_t retried[] = {0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t heartbeat[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    /* A TX_BUSY report names the actions channel without an action. */
  const uint8_t busy[] = {0x06u, 0x08u, 0x01u, 0x00u, 0xFFu};
  const uint8_t ack_resp[] = {0x08u, 0x00u};
  const uint8_t ack_filler[] = {0x08u, 0x01u};
  const uint8_t accepted_cancel[] = {0x07u, 0x02u, 0x00u, 0x00u};
  const uint8_t one[] = {0x00u};

  configure_three();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(8, 0, goal, sizeof goal);
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_answer_goal(1u, 5u, R2S_ACT_ACCEPTED));
    /* A second past the busy window: the retry timer resends the
     * outstanding response and the heartbeat goes out with it. */
  fake_port_advance(1000);
  fake_port_tx_clear();
  feed_host(8, 1, cancel, sizeof cancel);
  TEST_ASSERT_EQUAL_UINT8(0, g_cancel_calls);
  TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
  assert_tx(0, 8, 2, retried, sizeof retried);
  assert_tx(1, 4, 0, heartbeat, sizeof heartbeat);
  assert_tx(2, 5, 0, busy, sizeof busy);
    /* A held frame is not a refused one: the hold reports TX_BUSY but
     * drops nothing. */
  TEST_ASSERT_EQUAL_UINT16(0, r2s_framer_counters()->dropped_frames);
    /* The acks free both slots; the retried cancel then runs at once. */
  feed_host(6, 0, ack_resp, sizeof ack_resp);
  feed_host(6, 1, ack_filler, sizeof ack_filler);
  fake_port_tx_clear();
  feed_host(8, 1, cancel, sizeof cancel);
  TEST_ASSERT_EQUAL_UINT8(1, g_cancel_calls);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 8, 4, accepted_cancel, sizeof accepted_cancel);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_CANCELING, r2s_actions_state(0));
    /* Feedback still flows while canceling. */
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_actions_feedback(0u, 0u, one, sizeof one));
  TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
}

/* covers: ACT-8 */
void test_actions_cancel_for_an_unknown_goal_seq_answers_unknown(void)
{
  const uint8_t stranger[] = {0x06u, 0x01u, 0x07u};
  const uint8_t ack_goal[] = {0x08u, 0x00u};
  const uint8_t ack[] = {0x08u, 0x01u};
  const uint8_t unknown[] = {0x07u, 0x01u, 0x07u, 0x02u};

  start_goal();
  feed_host(6, 0, ack_goal, sizeof ack_goal);
  fake_port_tx_clear();
  feed_host(8, 1, stranger, sizeof stranger);
  TEST_ASSERT_EQUAL_UINT8(0, g_cancel_calls);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(0, 6, 1, ack, sizeof ack);
  assert_tx(1, 8, 2, unknown, sizeof unknown);
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
}

/* covers: REL-3 */
void test_actions_ack_without_a_match_is_ignored(void)
{
  const uint8_t empty[1] = {0u};
  const uint8_t ack[] = {0x08u, 0x00u};
  const uint8_t stray[] = {0x08u, 0x09u};
  const uint8_t accepted[] = {0x05u, 0x00u, 0x00u, 0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  feed_host(6, 0, ack, sizeof ack);
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
  start_goal();
  fake_port_tx_clear();
  feed_host(6, 0, stray, sizeof stray);
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_EXECUTING, r2s_actions_state(0));
    /* The stray ack cleared nothing: the retry still goes out on time, next
     * to the periodic heartbeat. */
  fake_port_advance(500);
  feed_host(4, 0, empty, 0);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(0, 8, 2, accepted, sizeof accepted);
}

/* covers: ACT-7, ACT-8, SYS-5 */
void test_actions_direct_frames_refuse_defensive_arms(void)
{
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t not_query[] = {0x09u, 0x00u};
  const uint8_t long_query[] = {0x01u, 0x00u, 0x00u, 0x00u};
  const uint8_t bad_rel_status[] = {0x03u, 0x01u, 0x00u, 0x00u};

  configure();
  fake_port_tx_clear();
  r2s_actions_on_frame((const uint8_t[]) {0x00u}, 0u, 0u, 0u);
  r2s_actions_on_frame(goal, sizeof goal, 0u, 0u);
  r2s_actions_on_frame(not_query, sizeof not_query, 0u, 0u);
  r2s_actions_on_frame(long_query, sizeof long_query, 0u, 0u);
  r2s_actions_on_frame(bad_rel_status, sizeof bad_rel_status, 0u, 0u);
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
  TEST_ASSERT_EQUAL_UINT8(0, g_goal_calls);
}

/* covers: REL-1, REL-2 */
void test_actions_blocked_link_holds_the_response_for_retry(void)
{
    /* GOAL rel 0, goal 0, action 0, order 5. */
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t accepted[] = {0x05u, 0x00u, 0x00u, 0x00u};

  configure();
  resolve_walk();
  connect();
  fake_port_tx_clear();
  fake_port_set_tx_free(0u);
  feed_host(8, 0, goal, sizeof goal);
  TEST_ASSERT_EQUAL_UINT8(1, g_goal_calls);
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    /* With space back the timer resends the response it never carried. */
  fake_port_set_tx_free(0xFFFFu);
  r2s_actions_poll(500u);
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 8, 1, accepted, sizeof accepted);
}

/* covers: ACT-9, REL-5 */
void test_actions_reset_returns_to_idle_with_fresh_seqs(void)
{
  const uint8_t goal[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t result[] = {0x00u};

  start_goal();
    /* The link timeout re-announces: states, statuses and streams restart. */
  fake_port_advance(2500);
  r2s_spin_some();
  TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_IDLE, r2s_actions_state(0));
  TEST_ASSERT_EQUAL_UINT8(R2S_ACT_WALK_UNKNOWN, r2s_actions_status(0));
    /* No RESULT ever answers the pre-reset goal. */
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED,
                           r2s_actions_result(0u, 0u, R2S_ACT_SUCCEEDED, result, sizeof result));
    /* The new handshake restarts the rel seq at 0. */
  {
    const uint8_t accepted[] = {0x05u, 0x00u, 0x00u, 0x00u};

    resolve_walk();
    connect();
    fake_port_tx_clear();
    feed_host(8, 0, goal, sizeof goal);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(1, 8, 1, accepted, sizeof accepted);
  }
}
