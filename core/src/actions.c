/* actions: the MCU side of the actions channel. The table is announced
 * through the walk, and one reliable stream per direction carries the goal
 * exchange while feedback stays best effort. */
#include "r2s/r2s_actions.h"

#include <string.h>

#include "r2s/r2s_arch.h"
#include "r2s/r2s_error.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"

#ifdef R2S_WITH_ACTIONS
/* A reliable payload waits this long for its ack before it goes out again,
 * and is dropped after this many retries (REL-2). */
#define REL_RETRY_MS 500u
#define REL_RETRIES 3u

static struct
{
  const r2s_action_t *table;
  uint8_t count;
  uint8_t table_in_flash;
  uint8_t state[R2S_ACTION_MAX_COUNT];
  uint8_t goal_seq[R2S_ACTION_MAX_COUNT];
  uint8_t status[R2S_ACTION_MAX_COUNT];
  uint8_t tx_seq_act;
  uint8_t rx_rel_expected;
  uint8_t tx_rel_next;
  uint8_t rel_out_active;
  uint8_t rel_out_rel;
  uint8_t rel_out_retries;
  uint32_t rel_out_deadline;
  uint16_t rel_out_len;
  uint8_t rel_pending_active;
  uint8_t rel_pending_rel;
  uint16_t rel_pending_len;
  uint8_t rel_out_payload[R2S_MAX_FRAME];
  uint8_t rel_pending_payload[R2S_MAX_FRAME];
} act;

/* One entry in RAM. A flash table goes byte by byte through
 * R2S_FLASH_READ_BYTE; a RAM table is a plain copy. The name strings stay in
 * RAM either way, so the copied pointers read as plain memory. */
static void table_copy(
  const r2s_action_t *table, uint8_t index, r2s_action_t *out,
  uint8_t from_flash)
{
  if (from_flash != 0u) {
    const uint8_t *src = (const uint8_t *)&table[index];
    uint8_t *dst = (uint8_t *)out;
    size_t i;

    for (i = 0u; i < sizeof(r2s_action_t); i++) {
      dst[i] = R2S_FLASH_READ_BYTE(src + i);
    }
  } else {
    *out = table[index];
  }
}

/* The DECLARE payload length of one entry, from its names. */
static uint32_t declare_need(const r2s_action_t *entry)
{
  uint8_t len_buf[R2S_VARINT_MAX_BYTES];
  size_t type_len = strlen(entry->type_name);
  size_t action_len = strlen(entry->action_name);

  return (uint32_t)10u + (uint32_t)r2s_varint_encode((uint16_t)type_len,
    len_buf) + (uint32_t)type_len +
         (uint32_t)r2s_varint_encode((uint16_t)action_len, len_buf) + (uint32_t)action_len;
}

static r2s_status_t bind(const r2s_action_t *actions, uint8_t count, uint8_t from_flash)
{
  uint8_t i;

  if ((actions == NULL && count != 0u) || count > R2S_ACTION_MAX_COUNT) {
    return R2S_ERR_BOUNDS;
  }
  for (i = 0u; i < count; i++) {
    r2s_action_t entry;

    table_copy(actions, i, &entry, from_flash);
    if (entry.type_name == NULL || entry.action_name == NULL) {
      return R2S_ERR_BOUNDS;
    }
    if (strlen(entry.type_name) > R2S_VARINT_MAX || strlen(entry.action_name) > R2S_VARINT_MAX) {
      return R2S_ERR_BOUNDS;
    }
        /* Opcode, index, hash, the length varints and the names: exact. The
         * sums run in 32 bits, because on a 16-bit size_t a name near 64 KB
         * would wrap a size_t sum back under the limit. */
    if (declare_need(&entry) > (uint32_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD)) {
      return R2S_ERR_BOUNDS;
    }
  }
  act.table = actions;
  act.count = count;
  act.table_in_flash = from_flash;
  for (i = 0u; i < count; i++) {
    act.status[i] = R2S_ACT_WALK_UNKNOWN;
    act.state[i] = R2S_ACT_IDLE;
    act.goal_seq[i] = 0u;
  }
  return R2S_OK;
}

r2s_status_t r2s_actions_configure(const r2s_action_t *actions, uint8_t count)
{
  return bind(actions, count, 1u);
}

r2s_status_t r2s_actions_configure_ram(const r2s_action_t *actions, uint8_t count)
{
  return bind(actions, count, 0u);
}

uint8_t r2s_actions_count(void)
{
  return act.count;
}

const r2s_action_t * r2s_actions_get(uint8_t index)
{
  if (index >= act.count) {
    return NULL;
  }
  return &act.table[index];
}

r2s_status_t r2s_actions_copy(uint8_t index, r2s_action_t *out)
{
  if (out == NULL || index >= act.count) {
    return R2S_ERR_BOUNDS;
  }
  table_copy(act.table, index, out, act.table_in_flash);
  return R2S_OK;
}

uint16_t r2s_actions_declare_len(uint8_t index)
{
  r2s_action_t entry;

  if (r2s_actions_copy(index, &entry) != R2S_OK) {
    return 0u;
  }
  return (uint16_t)declare_need(&entry);
}

uint8_t r2s_actions_state(uint8_t index)
{
  if (index >= act.count) {
    return R2S_ACT_IDLE;
  }
  return act.state[index];
}

uint8_t r2s_actions_goal_seq(uint8_t index)
{
  if (index >= act.count) {
    return 0u;
  }
  return act.goal_seq[index];
}

uint8_t r2s_actions_status(uint8_t index)
{
  if (index >= act.count) {
    return R2S_ACT_WALK_UNKNOWN;
  }
  return act.status[index];
}

/* Streams one payload on the actions channel with the next frame seq. */
static void send_bytes(const uint8_t *payload, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    (void)r2s_tx_byte(payload[i]);
  }
}

/* One reliable payload from its bytes, with the next frame seq (REL-4). A
 * frame that finds no space stays outstanding and due at once, without
 * counting a retry the link never carried. */
static void send_reliable_now(const uint8_t *payload, uint16_t len, uint32_t now)
{
  if (r2s_tx_begin(8u, act.tx_seq_act, len) != R2S_OK) {
    act.rel_out_deadline = now - ((uint32_t)R2S_TICK_HZ * (uint32_t)REL_RETRY_MS / 1000u);
    return;
  }
  send_bytes(payload, len);
    /* The begin reserved the frame and the loop feeds exactly its length,
     * while port errors never surface, so the end below cannot fail. */
  (void)r2s_tx_end();
  act.tx_seq_act++;
  act.rel_out_deadline = now;
}

/* The pending reliable payload becomes outstanding and goes out at once,
 * with a fresh timer and retry count. */
static void promote_pending(uint32_t now)
{
  uint16_t i;

  if (act.rel_pending_active == 0u) {
    return;
  }
  act.rel_out_active = 1u;
  act.rel_out_rel = act.rel_pending_rel;
  act.rel_out_retries = 0u;
  act.rel_out_len = act.rel_pending_len;
  for (i = 0u; i < act.rel_pending_len; i++) {
    act.rel_out_payload[i] = act.rel_pending_payload[i];
  }
  act.rel_pending_active = 0u;
  send_reliable_now(act.rel_out_payload, act.rel_out_len, now);
}

/* Whether these payload bytes fit the channel: the frame bound (FRM-11)
 * refuses the rest. The sum runs in 32 bits, because on a 16-bit size_t a
 * length near 64 KB would wrap a size_t sum back under the limit. */
static uint8_t fits_on_wire(uint32_t need)
{
  return need <= (uint32_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD);
}

/* Whether a reliable response has a slot: at most one reliable payload is
 * outstanding per direction (REL-2), and a second waits behind the first. */
static uint8_t response_slot_free(void)
{
  return act.rel_out_active == 0u || act.rel_pending_active == 0u;
}

/* Holds one reliable response for the retransmit timer: opcode, rel_seq,
 * goal seq, verdict and the optional body. The response carries the next
 * rel_seq of this direction's own stream (REL-3), stamped here instead of
 * in each caller. Staging into the retry buffers keeps the stack flat. */
static void queue_response(
  uint8_t op, uint8_t goal_seq, uint8_t tail, const uint8_t *body, uint16_t body_len,
  uint32_t now)
{
  uint8_t *slot;
  uint8_t rel;
  uint16_t len = (uint16_t)(4u + (uint32_t)body_len);
  uint16_t i;

  if (act.rel_out_active == 0u) {
    act.rel_out_active = 1u;
    rel = act.rel_out_rel = act.tx_rel_next++;
    act.rel_out_retries = 0u;
    act.rel_out_len = len;
    slot = act.rel_out_payload;
  } else {
    act.rel_pending_active = 1u;
    rel = act.rel_pending_rel = act.tx_rel_next++;
    act.rel_pending_len = len;
    slot = act.rel_pending_payload;
  }
  slot[0] = op;
  slot[1] = rel;
  slot[2] = goal_seq;
  slot[3] = tail;
  for (i = 0u; i < body_len; i++) {
    slot[4u + i] = body[i];
  }
  if (slot == act.rel_out_payload) {
    send_reliable_now(act.rel_out_payload, len, now);
  }
}

r2s_status_t r2s_actions_answer_goal(uint8_t index, uint8_t goal_seq, uint8_t decision)
{
  if (decision > R2S_ACT_UNKNOWN_ACTION) {
    return R2S_ERR_BOUNDS;
  }
  if (r2s_connected() == 0u) {
    r2s_error_note(R2S_ERROR_NOT_CONNECTED, 8u, 0xFFu, 1u);
    return R2S_ERR_NOT_CONNECTED;
  }
  if (index >= act.count || act.state[index] != R2S_ACT_IDLE) {
    return R2S_ERR_BOUNDS;
  }
  if (response_slot_free() == 0u) {
    return R2S_ERR_TX_BUSY;
  }
  queue_response(R2S_ACT_OP_GOAL_RESP, goal_seq, decision, NULL, 0u, r2s_port_tick());
  if (decision == R2S_ACT_ACCEPTED) {
    act.state[index] = R2S_ACT_EXECUTING;
    act.goal_seq[index] = goal_seq;
  }
  return R2S_OK;
}

r2s_status_t r2s_actions_answer_cancel(uint8_t index, uint8_t goal_seq, uint8_t decision)
{
  if (decision > R2S_ACT_CANCEL_UNKNOWN_GOAL) {
    return R2S_ERR_BOUNDS;
  }
  if (r2s_connected() == 0u) {
    r2s_error_note(R2S_ERROR_NOT_CONNECTED, 8u, 0xFFu, 1u);
    return R2S_ERR_NOT_CONNECTED;
  }
  if (index >= act.count || act.state[index] == R2S_ACT_IDLE || act.goal_seq[index] != goal_seq) {
    return R2S_ERR_BOUNDS;
  }
  if (response_slot_free() == 0u) {
    return R2S_ERR_TX_BUSY;
  }
  queue_response(R2S_ACT_OP_CANCEL_RESP, goal_seq, decision, NULL, 0u, r2s_port_tick());
    /* The checks above already matched the goal seq, so only the state can
     * still refuse the transition. */
  if (decision == R2S_ACT_CANCEL_ACCEPTED && act.state[index] == R2S_ACT_EXECUTING) {
    act.state[index] = R2S_ACT_CANCELING;
  }
  return R2S_OK;
}

r2s_status_t r2s_actions_feedback(
  uint8_t index, uint8_t goal_seq, const uint8_t *payload,
  uint16_t len)
{
  uint32_t need;

  if (r2s_connected() == 0u) {
    r2s_error_note(R2S_ERROR_NOT_CONNECTED, 8u, 0xFFu, 1u);
    return R2S_ERR_NOT_CONNECTED;
  }
  if (index >= act.count || act.goal_seq[index] != goal_seq ||
    (act.state[index] != R2S_ACT_EXECUTING && act.state[index] != R2S_ACT_CANCELING))
  {
    return R2S_ERR_BOUNDS;
  }
  if (payload == NULL && len != 0u) {
    return R2S_ERR_BOUNDS;
  }
    /* Opcode, goal seq, action index and the packed Feedback. */
  need = (uint32_t)3u + (uint32_t)len;
  if (fits_on_wire(need) == 0u) {
    return R2S_ERR_BOUNDS;
  }
  if (r2s_tx_begin(8u, act.tx_seq_act, (uint16_t)need) != R2S_OK) {
    return R2S_ERR_TX_BUSY;
  }
  (void)r2s_tx_byte(R2S_ACT_OP_FEEDBACK);
  (void)r2s_tx_byte(goal_seq);
  (void)r2s_tx_byte(index);
  send_bytes(payload, len);
  (void)r2s_tx_end();
  act.tx_seq_act++;
  return R2S_OK;
}

r2s_status_t r2s_actions_result(
  uint8_t index, uint8_t goal_seq, uint8_t outcome, const uint8_t *payload,
  uint16_t len)
{
  uint32_t need;

  if (outcome != R2S_ACT_SUCCEEDED && outcome != R2S_ACT_CANCELED && outcome != R2S_ACT_ABORTED) {
    return R2S_ERR_BOUNDS;
  }
  if (r2s_connected() == 0u) {
    r2s_error_note(R2S_ERROR_NOT_CONNECTED, 8u, 0xFFu, 1u);
    return R2S_ERR_NOT_CONNECTED;
  }
  if (index >= act.count || act.state[index] == R2S_ACT_IDLE || act.goal_seq[index] != goal_seq) {
    return R2S_ERR_BOUNDS;
  }
  if (payload == NULL && len != 0u) {
    return R2S_ERR_BOUNDS;
  }
    /* Opcode, rel seq, goal seq, outcome and the packed Result. */
  need = (uint32_t)4u + (uint32_t)len;
  if (fits_on_wire(need) == 0u) {
    return R2S_ERR_BOUNDS;
  }
  if (response_slot_free() == 0u) {
    return R2S_ERR_TX_BUSY;
  }
  queue_response(R2S_ACT_OP_RESULT, goal_seq, outcome, payload, len, r2s_port_tick());
  act.state[index] = R2S_ACT_IDLE;
  act.goal_seq[index] = 0u;
  return R2S_OK;
}

/* One plain DECLARE answering a QUERY (ACT-7), streamed byte by byte so no
 * buffer holds the frame. */
static void send_declare(uint8_t index)
{
  r2s_action_t entry;
  uint16_t len;
  size_t type_len;
  size_t action_len;
  uint16_t i;

    /* on_frame answers a QUERY for an announced index only, so the copy
     * holds and the length below is not zero. */
  (void)r2s_actions_copy(index, &entry);
  len = r2s_actions_declare_len(index);
  if (r2s_tx_begin(8u, act.tx_seq_act, len) != R2S_OK) {
    return;
  }
  type_len = strlen(entry.type_name);
  action_len = strlen(entry.action_name);
  (void)r2s_tx_byte(R2S_ACT_OP_DECLARE);
  (void)r2s_tx_byte(index);
  for (i = 0u; i < 8u; i++) {
    (void)r2s_tx_byte(entry.type_hash[i]);
  }
  {
    uint8_t varint[R2S_VARINT_MAX_BYTES];
    uint8_t n = r2s_varint_encode((uint16_t)type_len, varint);
    uint8_t k;

    for (k = 0u; k < n; k++) {
      (void)r2s_tx_byte(varint[k]);
    }
  }
  for (i = 0u; i < (uint16_t)type_len; i++) {
    (void)r2s_tx_byte((uint8_t)entry.type_name[i]);
  }
  {
    uint8_t varint[R2S_VARINT_MAX_BYTES];
    uint8_t n = r2s_varint_encode((uint16_t)action_len, varint);
    uint8_t k;

    for (k = 0u; k < n; k++) {
      (void)r2s_tx_byte(varint[k]);
    }
  }
  for (i = 0u; i < (uint16_t)action_len; i++) {
    (void)r2s_tx_byte((uint8_t)entry.action_name[i]);
  }
    /* The begin reserved the frame and the loop feeds exactly its length,
     * while port errors never surface, so the end below cannot fail. */
  (void)r2s_tx_end();
  act.tx_seq_act++;
}

/* One plain STATUS answering a QUERY past the table (ACT-7): under the
 * request retry, never a reliable frame. */
static void send_plain_status(uint8_t index)
{
  uint8_t payload[4];

  payload[0] = R2S_ACT_OP_STATUS;
  payload[1] = 0u;
  payload[2] = index;
  payload[3] = 1u;
  if (r2s_tx_begin(8u, act.tx_seq_act, sizeof payload) != R2S_OK) {
    return;
  }
  send_bytes(payload, sizeof payload);
  (void)r2s_tx_end();
  act.tx_seq_act++;
}

/* The walk: one QUERY per index, answered with DECLARE or STATUS 1, and the
 * host's STATUS verdict on each DECLARE (ACT-7). */
static void on_walk(const uint8_t *payload, uint16_t len)
{
  if (len == 2u && payload[0] == R2S_ACT_OP_QUERY) {
    if (payload[1] < act.count) {
      send_declare(payload[1]);
    } else {
      send_plain_status(payload[1]);
    }
    return;
  }
  if (len == 4u && payload[0] == R2S_ACT_OP_STATUS && payload[1] == 0u && payload[2] < act.count &&
    payload[3] <= R2S_ACT_STATUS_NAME_CONFLICT)
  {
    act.status[payload[2]] = payload[3];
    return;
  }
  r2s_framer_count_refused();
}

/* The first action holding this goal_seq, or 0xFF when no active goal does:
 * CANCEL and RESULT name only the goal, never its action (ACT-2). */
static uint8_t find_goal(uint8_t goal_seq)
{
  uint8_t i;

  for (i = 0u; i < act.count; i++) {
    if (act.state[i] != R2S_ACT_IDLE && act.goal_seq[i] == goal_seq) {
      return i;
    }
  }
  return 0xFFu;
}

/* Answers a GOAL that the checks below refuse without the C++ layer: an
 * unknown or unresolved action, or one that already runs a goal (ACT-4,
 * ACT-8). The caller acked first, so the answer rides its own rel_seq. */
static void refuse_goal(uint8_t goal_seq, uint8_t index, uint32_t now)
{
  uint8_t decision = (index >= act.count ||
    act.status[index] != R2S_ACT_STATUS_OK) ? R2S_ACT_UNKNOWN_ACTION :
    R2S_ACT_BUSY;

  queue_response(R2S_ACT_OP_GOAL_RESP, goal_seq, decision, NULL, 0u, now);
}

/* The goal exchange while CONNECTED: GOAL takes IDLE through the C++ layer
 * and answers busy past it, CANCEL reaches the layer for the matching active
 * goal (ACT-8). Anything else on the channel is refused. */
static void on_exchange(const uint8_t *payload, uint16_t len, uint32_t now)
{
  uint8_t rel;
  uint8_t index;

  if (len < 3u) {
    r2s_framer_count_refused();
    return;
  }
  if (payload[0] == R2S_ACT_OP_GOAL) {
    uint8_t goal_seq;

    if (len < 4u) {
      r2s_framer_count_refused();
      return;
    }
    rel = payload[1];
    goal_seq = payload[2];
    index = payload[3];
        /* Every reliable frame is acked, duplicates included, and the payload
         * applies at most once: only the expected rel_seq applies and
         * advances the counter (REL-3). */
    if (rel != act.rx_rel_expected) {
      r2s_session_send_ack(8u, rel);
      return;
    }
        /* The answer rides the same reliable stream: when both slots are
         * full no answer can go out, so the frame stays unacked and the
         * host's retry brings it back to a free slot (REL-2). Acking it
         * first would consume the retry while the answer is lost. A held
         * frame is not a refused one: only the TX_BUSY note reports it. */
    if (response_slot_free() == 0u) {
      r2s_error_note(R2S_ERROR_TX_BUSY, 8u, index, 1u);
      return;
    }
    r2s_session_send_ack(8u, rel);
    act.rx_rel_expected++;
    if (index >= act.count || act.status[index] != R2S_ACT_STATUS_OK ||
      act.state[index] != R2S_ACT_IDLE)
    {
      refuse_goal(goal_seq, index, now);
      return;
    }
    r2s_action_on_goal(index, goal_seq, payload + 4u, (uint16_t)(len - 4u));
    return;
  }
  if (payload[0] == R2S_ACT_OP_CANCEL) {
    uint8_t goal_seq;

    if (len != 3u) {
      r2s_framer_count_refused();
      return;
    }
    rel = payload[1];
    goal_seq = payload[2];
    if (rel != act.rx_rel_expected) {
      r2s_session_send_ack(8u, rel);
      return;
    }
        /* As above: no answer can go out with both slots full, so the frame
         * stays unacked for the host's retry. A held frame is not a
         * refused one: only the TX_BUSY note reports it. */
    if (response_slot_free() == 0u) {
      r2s_error_note(R2S_ERROR_TX_BUSY, 8u, 0xFFu, 1u);
      return;
    }
    r2s_session_send_ack(8u, rel);
    act.rx_rel_expected++;
    index = find_goal(goal_seq);
    if (index == 0xFFu) {
      queue_response(R2S_ACT_OP_CANCEL_RESP, goal_seq, R2S_ACT_CANCEL_UNKNOWN_GOAL, NULL, 0u, now);
      return;
    }
    r2s_action_on_cancel(index, goal_seq);
    return;
  }
  r2s_framer_count_refused();
}

void r2s_actions_on_frame(const uint8_t *payload, uint16_t len, uint32_t now, uint8_t connected)
{
  if (len == 0u) {
    r2s_framer_count_refused();
    return;
  }
    /* The walk owns every announcing frame: a non-walk opcode is refused
     * there, next to the short and mispredicted ones. */
  if (connected == 0u) {
    on_walk(payload, len);
    return;
  }
  on_exchange(payload, len, now);
}

void r2s_actions_on_ack(uint8_t rel_seq, uint32_t now)
{
  if (act.rel_out_active != 0u && rel_seq == act.rel_out_rel) {
    act.rel_out_active = 0u;
    promote_pending(now);
  }
}

/* The 500 ms timer: resends the outstanding reliable payload, or drops and
 * counts it after 3 retries (REL-2). A pending response moves up either
 * way, so no answer is left unreported. */
void r2s_actions_poll(uint32_t now)
{
  if (act.rel_out_active == 0u ||
    (uint32_t)(now - act.rel_out_deadline) < (uint32_t)R2S_TICK_HZ / 2u)
  {
    return;
  }
  if (act.rel_out_retries >= REL_RETRIES) {
    act.rel_out_active = 0u;
    r2s_framer_count_refused();
    promote_pending(now);
    return;
  }
  act.rel_out_retries++;
  send_reliable_now(act.rel_out_payload, act.rel_out_len, now);
}

void r2s_actions_note_reset(void)
{
  uint8_t i;

  for (i = 0u; i < R2S_ACTION_MAX_COUNT; i++) {
    act.state[i] = R2S_ACT_IDLE;
    act.goal_seq[i] = 0u;
    act.status[i] = R2S_ACT_WALK_UNKNOWN;
  }
  act.tx_seq_act = 0u;
  act.rx_rel_expected = 0u;
  act.tx_rel_next = 0u;
  act.rel_out_active = 0u;
  act.rel_out_retries = 0u;
  act.rel_pending_active = 0u;
}

/* Without the C++ layer no goal work runs: the defaults refuse instead of
 * running none. */
__attribute__((weak)) void r2s_action_on_goal(
  uint8_t index, uint8_t goal_seq, const uint8_t *payload,
  uint16_t len)
{
  (void)payload;
  (void)len;
  (void)r2s_actions_answer_goal(index, goal_seq, R2S_ACT_REJECTED);
}

__attribute__((weak)) void r2s_action_on_cancel(uint8_t index, uint8_t goal_seq)
{
  (void)r2s_actions_answer_cancel(index, goal_seq, R2S_ACT_CANCEL_REJECTED);
}

#ifdef R2S_TESTING
void r2s_actions_test_reset(void)
{
  act.table = NULL;
  act.count = 0u;
  act.table_in_flash = 0u;
  r2s_actions_note_reset();
}
#endif
#else
/* ISO C needs at least one declaration in a translation unit. */
typedef int r2s_actions_unused;
#endif
