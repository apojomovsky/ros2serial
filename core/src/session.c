// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* session: the MCU side of the handshake and the link. It announces the
 * firmware on boot, answers the host through ANNOUNCING, and sends heartbeats
 * while CONNECTED (spec/protocol.md, sections 7 and 8). */
#include "r2s/r2s_session.h"

#include <float.h>
#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_error.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"
#ifdef R2S_WITH_PARAMS
#include "r2s/r2s_params.h"
#endif
#ifdef R2S_WITH_SERVICES
#include "r2s/r2s_services.h"
#endif
#ifdef R2S_WITH_ACTIONS
#include "r2s/r2s_actions.h"
#endif

#ifdef R2S_WITH_CODEC
#include "r2s/r2s_codec.h"
#endif
#ifdef R2S_WITH_TIME
#include "r2s/r2s_time.h"
#endif

/* The link states (SES-1, SES-6, SES-7). */
#define ANNOUNCING 0u
#define CONNECTED 1u

/* Control opcodes (CTL-16). */
#define OP_HELLO_REQ 0x01u
#define OP_HELLO 0x02u
#define OP_TOPIC_REQ 0x03u
#define OP_TOPIC 0x04u
#define OP_TOPIC_STATUS 0x05u
#define OP_READY 0x06u

/* Time opcodes (TIM-10). */
#define OP_PING 0x01u
#define OP_PONG 0x02u
#define OP_CORRECTION 0x03u

/* ANNOUNCING repeats HELLO this often without host traffic (SES-5), and
 * answers other host frames with HELLO at most this often (SES-3). */
#define HELLO_REPEAT_MS 1000u
#define HELLO_NACK_MS 100u

/* The CTL-2 feature bits this build sets: bit 0 for time, bit 1 for logging,
 * bits 2 and 3 for parameters and the reliable channel, bits 3 and 7 for
 * actions and the reliable channel; bit 4 is informational. Each bit rides
 * on the switch of the calls it announces. */
#if DBL_MANT_DIG == FLT_MANT_DIG
#define R2S_FEATURE_BASE 0x10u
#else
#define R2S_FEATURE_BASE 0x00u
#endif
#ifdef R2S_WITH_LOG
#define R2S_FEATURE_LOG 0x02u
#else
#define R2S_FEATURE_LOG 0x00u
#endif
#ifdef R2S_WITH_TIME
#define R2S_FEATURE_TIME 0x01u
#else
#define R2S_FEATURE_TIME 0x00u
#endif
#ifdef R2S_WITH_PARAMS
#define R2S_FEATURE_PARAMS 0x0Cu
#else
#define R2S_FEATURE_PARAMS 0x00u
#endif
#ifdef R2S_WITH_SERVICES
#define R2S_FEATURE_SERVICES 0x40u
#else
#define R2S_FEATURE_SERVICES 0x00u
#endif
#ifdef R2S_WITH_ACTIONS
#define R2S_FEATURE_ACTIONS 0x88u
#else
#define R2S_FEATURE_ACTIONS 0x00u
#endif
#define R2S_FEATURE_BITS (R2S_FEATURE_BASE | R2S_FEATURE_LOG | R2S_FEATURE_TIME | \
  R2S_FEATURE_PARAMS | R2S_FEATURE_SERVICES | R2S_FEATURE_ACTIONS)

/* tick_hz is 1000 times a divisor of 10^6, so a tick converts to nanoseconds
 * with integer math alone (CTL-8). A failing build names the rule here. */
typedef char r2s_tick_hz_form[(R2S_TICK_HZ >= 1000u && R2S_TICK_HZ % 1000u == 0u &&
    (1000000u % (R2S_TICK_HZ / 1000u)) == 0u) ?
    1 :
    -1];

/* The bound table holds at most R2S_TOPIC_MAX_COUNT entries, and the session
 * indexes its R2S_SESSION_MAX_TOPICS arrays by accessor index: the arrays
 * cover every bindable count only when the bindable maximum fits them. */
typedef char r2s_topic_max_within_session_max[(R2S_TOPIC_MAX_COUNT <=
    R2S_SESSION_MAX_TOPICS) ? 1 : -1];

/* A STATUS payload is its opcode, rel_seq, index and verdict. */
#define REL_STATUS_LEN 4u

/* A reliable payload waits this long for its ack before it goes out again,
 * and is dropped after this many retries (REL-2). */
#define REL_RETRY_MS 500u
#define REL_RETRIES 3u

static struct
{
  uint8_t state;
  const char *node_name;
  uint8_t status[R2S_SESSION_MAX_TOPICS];
  uint8_t tx_seq[R2S_SESSION_MAX_TOPICS];
  uint8_t rx_expected[R2S_SESSION_MAX_TOPICS];
  uint16_t rx_lost[R2S_SESSION_MAX_TOPICS];
  uint8_t tx_seq_ctrl;
  uint8_t tx_seq_hb;
#if defined(R2S_WITH_PARAMS) || defined(R2S_WITH_SERVICES) || defined(R2S_WITH_ACTIONS)
    /* The ack channel's frame seq, shared by every reliable channel: one
     * counter serves the acks the parameters, the services and the actions
     * send (REL-4). */
  uint8_t tx_seq_ack;
#endif
  uint8_t tx_seq_time;
  uint32_t token;
  uint32_t last_host_tick;
  uint32_t last_hello_tick;
  uint32_t last_hb_tick;
  uint32_t last_nack_tick;
  uint8_t heard;
#ifdef R2S_WITH_PARAMS
    /* The reliable channel (REL-1 to REL-5): the frame seq of the parameter
     * channel, the next expected SET rel_seq and the next STATUS one, and
     * the outstanding STATUS with its timer and retries. A second applied
     * SET waits in the pending slot while the first is outstanding; a
     * conforming host never fills it. */
  uint8_t tx_seq_param;
  uint8_t rx_rel_expected;
  uint8_t tx_rel_next;
  uint8_t rel_out_active;
  uint8_t rel_out_rel;
  uint8_t rel_out_retries;
  uint32_t rel_out_deadline;
  uint8_t rel_out_payload[REL_STATUS_LEN];
  uint8_t rel_pending_active;
  uint8_t rel_pending_rel;
  uint8_t rel_pending_payload[REL_STATUS_LEN];
#endif
} session = {.node_name = ""};

static uint32_t ms_to_ticks(uint32_t ms)
{
    /* On 16-bit ints (AVR) ms * R2S_TICK_HZ would overflow, so the product is
     * 32-bit: at most 2000 * 10^6, which fits. */
  return (ms * (uint32_t)R2S_TICK_HZ) / 1000u;
}

static uint8_t varint_len(uint16_t value)
{
  uint8_t out[R2S_VARINT_MAX_BYTES];

  return r2s_varint_encode(value, out);
}

static r2s_status_t bind(
  const char *node_name, const r2s_topic_t *topics, uint8_t topic_count,
  uint8_t table_in_flash)
{
  size_t name_len;
  r2s_status_t rc;

  if (node_name == NULL) {
    return R2S_ERR_BOUNDS;
  }
  name_len = strlen(node_name);
  if (name_len > R2S_VARINT_MAX) {
    return R2S_ERR_BOUNDS;
  }
    /* 17 fixed bytes, the length varint and the name: exact, so names of 128
     * bytes or more, whose varint takes two bytes, fit exactly. The sums run
     * in 32 bits, because on a 16-bit size_t a name near 64 KB would wrap a
     * size_t sum back under the limit. */
  if ((uint32_t)17u + (uint32_t)varint_len((uint16_t)name_len) + (uint32_t)name_len >
    (uint32_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD))
  {
    return R2S_ERR_BOUNDS;
  }
    /* The bind is the single gate for the table: it validates names and
     * sizes, and binds only a table it accepts, so a refusal below changes
     * nothing announced. Flash and RAM tables bind through their own entry
     * point, because their addresses overlap on AVR. */
  if (table_in_flash != 0u) {
    rc = r2s_topics_configure(topics, topic_count);
  } else {
    rc = r2s_topics_configure_ram(topics, topic_count);
  }
  if (rc != R2S_OK) {
    return R2S_ERR_BOUNDS;
  }
  session.node_name = node_name;
  return R2S_OK;
}

r2s_status_t r2s_session_configure(
  const char *node_name, const r2s_topic_t *topics,
  uint8_t topic_count)
{
  return bind(node_name, topics, topic_count, 1u);
}

r2s_status_t r2s_session_configure_ram(
  const char *node_name, const r2s_topic_t *topics,
  uint8_t topic_count)
{
  return bind(node_name, topics, topic_count, 0u);
}

uint8_t r2s_session_find_topic(uint8_t topic_id)
{
  return r2s_topic_find(topic_id);
}

uint8_t r2s_session_topic_flags(uint8_t index)
{
  r2s_topic_t topic;

  if (r2s_topic_copy(index, &topic) != R2S_OK) {
    return 0u;
  }
  return topic.flags;
}

uint8_t r2s_session_topic_status(uint8_t index)
{
  return session.status[index];
}

uint32_t r2s_session_token(void)
{
  return session.token;
}

uint8_t r2s_session_tx_seq(uint8_t index)
{
  return session.tx_seq[index];
}

void r2s_session_advance_tx_seq(uint8_t index)
{
  session.tx_seq[index] = (uint8_t)(session.tx_seq[index] + 1u);
}

uint16_t r2s_session_note_rx_seq(uint8_t index, uint8_t seq)
{
    /* Both differences wrap mod 256 in uint8_t, which is the count [FRM-15]
     * names; the widening sum keeps every gap. */
  session.rx_lost[index] += (uint16_t)(uint8_t)(seq - session.rx_expected[index]);
  session.rx_expected[index] = (uint8_t)(seq + 1u);
  return session.rx_lost[index];
}

uint16_t r2s_session_rx_lost(uint8_t index)
{
  return session.rx_lost[index];
}

#ifdef R2S_WITH_PARAMS
/* A re-handshake drops all reliable state on both sides (REL-5). */
static void drop_reliable(void)
{
  session.tx_seq_param = 0u;
  session.tx_seq_ack = 0u;
  session.rx_rel_expected = 0u;
  session.tx_rel_next = 0u;
  session.rel_out_active = 0u;
  session.rel_out_retries = 0u;
  session.rel_pending_active = 0u;
}
#endif

static void enter_announcing(uint32_t now)
{
  uint8_t i;

  session.state = ANNOUNCING;
  session.heard = 0u;
  for (i = 0u; i < R2S_SESSION_MAX_TOPICS; i++) {
    session.status[i] = 0xFFu;
  }
    /* The seq counters restart with the handshake ([SES-11]); the lost count
     * stays, since it counts frames since boot ([FRM-15]). */
  for (i = 0u; i < r2s_topic_count(); i++) {
    session.tx_seq[i] = 0u;
    session.rx_expected[i] = 0u;
  }
#ifdef R2S_WITH_PARAMS
  drop_reliable();
#endif
#ifdef R2S_WITH_SERVICES
  r2s_services_note_reconnect();
#endif
#ifdef R2S_WITH_ACTIONS
#if !defined(R2S_WITH_PARAMS)
  session.tx_seq_ack = 0u;
#endif
  r2s_actions_note_reset();
#endif
  session.last_hello_tick = now;
  session.last_nack_tick = now - ms_to_ticks(HELLO_NACK_MS);
}

static void enter_connected(uint32_t now)
{
  session.state = CONNECTED;
  session.tx_seq_ctrl = 0u;
  session.tx_seq_hb = 0u;
#ifdef R2S_WITH_PARAMS
  drop_reliable();
#endif
#ifdef R2S_WITH_SERVICES
  r2s_services_note_reconnect();
#endif
#ifdef R2S_WITH_TIME
  session.tx_seq_time = 0u;
#endif
#ifdef R2S_WITH_LOG
  r2s_log_note_connected();
#endif
    /* The report sequence restarts with the handshake, and a new connection
     * may report a not-connected failure again; failures the old connection
     * counted flush now. */
  r2s_error_note_connected();
  session.last_hb_tick = now;
}

static void put_byte(uint8_t byte)
{
  (void)r2s_tx_byte(byte);
}

static void put_u16(uint16_t value)
{
  put_byte((uint8_t)(value & 0xFFu));
  put_byte((uint8_t)((value >> 8u) & 0xFFu));
}

static void put_u32(uint32_t value)
{
  put_byte((uint8_t)(value & 0xFFu));
  put_byte((uint8_t)((value >> 8u) & 0xFFu));
  put_byte((uint8_t)((value >> 16u) & 0xFFu));
  put_byte((uint8_t)((value >> 24u) & 0xFFu));
}

static void put_varint(uint16_t value)
{
  uint8_t out[R2S_VARINT_MAX_BYTES];
  uint8_t n = r2s_varint_encode(value, out);
  uint8_t i;

  for (i = 0u; i < n; i++) {
    put_byte(out[i]);
  }
}

static void put_string(const char *text)
{
    /* configure() bounds every name, so the cast cannot truncate. */
  uint16_t len = (uint16_t)strlen(text);
  const uint8_t *bytes = (const uint8_t *)text;
  uint16_t i;

  put_varint(len);
  for (i = 0u; i < len; i++) {
    put_byte(bytes[i]);
  }
}

/* The HELLO payload is 17 fixed bytes, the name length varint and the name.
 * configure() bounds the name, so the casts cannot truncate. */
static uint16_t hello_len(void)
{
  size_t name_len = strlen(session.node_name);
  uint32_t len = (uint32_t)17u + (uint32_t)varint_len((uint16_t)name_len) + (uint32_t)name_len;

  return (uint16_t)len;
}

static void send_hello(void)
{
  if (r2s_tx_begin(0u, session.tx_seq_ctrl, hello_len()) != R2S_OK) {
    return;
  }
  put_byte(OP_HELLO);
  put_byte(R2S_PROTOCOL_VERSION);
  put_byte(0x00u);
  put_u32(session.token);
  put_u16(R2S_MAX_FRAME);
  put_u16(R2S_FRAMER_RX_CAPACITY);
  put_u32(R2S_TICK_HZ);
  put_byte(R2S_FEATURE_BITS);
  put_byte(r2s_topic_count());
  put_string(session.node_name);
  if (r2s_tx_end() == R2S_OK) {
    session.tx_seq_ctrl++;
  }
}

static void send_topic(uint8_t index)
{
  r2s_topic_t topic;
  uint16_t type_len;
  uint16_t topic_len;
  uint16_t len;
  uint8_t i;

    /* on_control refuses an index past the count, so the copy holds. */
  (void)r2s_topic_copy(index, &topic);
  type_len = (uint16_t)strlen(topic.type_name);
  topic_len = (uint16_t)strlen(topic.topic_name);
  len = (uint16_t)(12u + varint_len(type_len) + type_len + varint_len(topic_len) + topic_len);

  if (r2s_tx_begin(0u, session.tx_seq_ctrl, len) != R2S_OK) {
    return;
  }
  put_byte(OP_TOPIC);
  put_byte(index);
  put_byte(topic.topic_id);
  put_byte(topic.flags);
  for (i = 0u; i < 8u; i++) {
    put_byte(topic.type_hash[i]);
  }
  put_string(topic.type_name);
  put_string(topic.topic_name);
  if (r2s_tx_end() == R2S_OK) {
    session.tx_seq_ctrl++;
  }
}

static void send_heartbeat(void)
{
  const r2s_framer_counters_t *count = r2s_framer_counters();

  if (r2s_tx_begin(4u, session.tx_seq_hb, 8u) != R2S_OK) {
    return;
  }
  put_u16(count->rx_frames);
  put_u16(count->crc_errors);
  put_u16(count->rx_overflows);
  put_u16(count->dropped_frames);
  if (r2s_tx_end() == R2S_OK) {
    session.tx_seq_hb++;
  }
}

static void refuse(void)
{
  r2s_framer_count_refused();
}

/* Answers a non-handshake host frame, defined below; the reliable channel
 * calls it for frames that need no other answer. */
static void answer_with_hello(uint32_t now);

#if defined(R2S_WITH_PARAMS) || defined(R2S_WITH_SERVICES) || defined(R2S_WITH_ACTIONS)
/* One ack for the reliable frame received, on the ack channel with the
 * channel and rel_seq it carried (REL-1). When the link cannot take bytes
 * the ack is dropped and the busy transmitter reported (REL-6): the
 * sender's retry brings the frame back. The parameters, the services and
 * the actions channels share it, each on its own rel_seq stream. */
static void send_ack(uint8_t channel, uint8_t rel_seq)
{
  if (r2s_tx_begin(6u, session.tx_seq_ack, 2u) != R2S_OK) {
    r2s_error_note(R2S_ERROR_TX_BUSY, channel, 0xFFu, 1u);
    return;
  }
  put_byte(channel);
  put_byte(rel_seq);
  if (r2s_tx_end() == R2S_OK) {
    session.tx_seq_ack++;
  }
}
#endif

#ifdef R2S_WITH_ACTIONS
/* The actions channel's ack: the single ack counter serves every reliable
 * channel, so the actions layer sends through here instead of its own. */
void r2s_session_send_ack(uint8_t channel, uint8_t rel_seq)
{
  send_ack(channel, rel_seq);
}
#endif

#ifdef R2S_WITH_PARAMS
/* One STATUS frame from its payload, with the next parameter frame seq
 * (REL-4). A frame that finds no space stays outstanding and due at once,
 * without counting a retry the link never carried. */
static void send_status_now(const uint8_t *payload, uint32_t now)
{
  uint8_t i;

  if (r2s_tx_begin(3u, session.tx_seq_param, REL_STATUS_LEN) != R2S_OK) {
    session.rel_out_deadline = now - ms_to_ticks(REL_RETRY_MS);
    return;
  }
  for (i = 0u; i < REL_STATUS_LEN; i++) {
    put_byte(payload[i]);
  }
    /* The begin reserved the frame and the loop feeds exactly its length,
     * while port errors never surface, so the end below cannot fail. */
  (void)r2s_tx_end();
  session.tx_seq_param++;
  session.rel_out_deadline = now;
}

/* The pending STATUS becomes outstanding and goes out at once, with a fresh
 * timer and retry count. */
static void promote_status(uint32_t now)
{
  uint8_t i;

  if (session.rel_pending_active == 0u) {
    return;
  }
  session.rel_out_active = 1u;
  session.rel_out_rel = session.rel_pending_rel;
  session.rel_out_retries = 0u;
  for (i = 0u; i < REL_STATUS_LEN; i++) {
    session.rel_out_payload[i] = session.rel_pending_payload[i];
  }
  session.rel_pending_active = 0u;
  send_status_now(session.rel_out_payload, now);
}

/* Holds the STATUS for the retransmit timer: at most one reliable payload
 * is outstanding per direction (REL-2). The STATUS carries the next rel_seq
 * of this direction's own stream (REL-3), which moves in lockstep with the
 * host's under stop-and-wait. A second applied SET waits behind the first;
 * a conforming host never fills that slot. */
static void queue_status(uint8_t index, uint8_t status, uint32_t now)
{
  uint8_t payload[REL_STATUS_LEN];
  uint8_t status_rel = session.tx_rel_next++;

  payload[0] = R2S_PARAM_OP_STATUS;
  payload[1] = status_rel;
  payload[2] = index;
  payload[3] = status;
  if (session.rel_out_active == 0u) {
    uint8_t i;

    session.rel_out_active = 1u;
    session.rel_out_rel = status_rel;
    session.rel_out_retries = 0u;
    for (i = 0u; i < REL_STATUS_LEN; i++) {
      session.rel_out_payload[i] = payload[i];
    }
    send_status_now(session.rel_out_payload, now);
  } else {
    uint8_t i;

    session.rel_pending_active = 1u;
    session.rel_pending_rel = status_rel;
    for (i = 0u; i < REL_STATUS_LEN; i++) {
      session.rel_pending_payload[i] = payload[i];
    }
  }
}

/* One plain DECLARE answering a PARAM_REQ (PAR-7, PAR-8), streamed byte by
 * byte so no buffer holds the frame. The default travels as the table
 * holds it: the scalar width, or the length varint and the bytes. */
static void send_declare(uint8_t index)
{
  const r2s_param_t *param = r2s_params_get(index);
  uint16_t len;
  size_t name_len;
  uint16_t i;

    /* The caller answers a PARAM_REQ for an announced index, so the entry
     * exists and its DECLARE does not fit in zero bytes. */
  len = r2s_params_declare_len(index);
  if (r2s_tx_begin(3u, session.tx_seq_param, len) != R2S_OK) {
    return;
  }
  name_len = strlen(param->name);
  put_byte(R2S_PARAM_OP_DECLARE);
  put_byte(index);
  put_byte(param->type);
  put_byte(param->flags);
  put_varint((uint16_t)name_len);
  for (i = 0u; i < (uint16_t)name_len; i++) {
    put_byte((uint8_t)param->name[i]);
  }
  if (param->type == R2S_PARAM_BOOL) {
    put_byte(param->initial.b);
  } else if (param->type == R2S_PARAM_I64) {
        /* The bits move as integers, never as float arithmetic. */
    uint64_t bits = 0u;

    memcpy(&bits, &param->initial.i, 8u);
    for (i = 0u; i < 8u; i++) {
      put_byte((uint8_t)(bits >> (i * 8u)));
    }
  } else if (param->type == R2S_PARAM_F64) {
    uint64_t bits = 0u;

    memcpy(&bits, &param->initial.f, 8u);
    for (i = 0u; i < 8u; i++) {
      put_byte((uint8_t)(bits >> (i * 8u)));
    }
  } else {
    size_t text_len = strlen(param->initial.s);

    put_varint((uint16_t)text_len);
    for (i = 0u; i < (uint16_t)text_len; i++) {
      put_byte((uint8_t)param->initial.s[i]);
    }
  }
    /* The begin reserved the frame and the loop feeds exactly its length,
     * while port errors never surface, so the end below cannot fail. */
  (void)r2s_tx_end();
  session.tx_seq_param++;
}

/* One plain STATUS answering a PARAM_REQ past the table (PAR-7, PAR-8):
 * under the request retry, never a reliable frame, rel_seq 0. */
static void send_plain_status(uint8_t index, uint8_t status)
{
  uint8_t payload[REL_STATUS_LEN];
  uint8_t i;

  payload[0] = R2S_PARAM_OP_STATUS;
  payload[1] = 0u;
  payload[2] = index;
  payload[3] = status;
  if (r2s_tx_begin(3u, session.tx_seq_param, REL_STATUS_LEN) != R2S_OK) {
    return;
  }
  for (i = 0u; i < REL_STATUS_LEN; i++) {
    put_byte(payload[i]);
  }
    /* The begin reserved the frame and the loop feeds exactly its length,
     * while port errors never surface, so the end below cannot fail. */
  (void)r2s_tx_end();
  session.tx_seq_param++;
}

static void on_params(const uint8_t *payload, uint16_t len, uint32_t now)
{
  uint8_t rel_seq;
  uint8_t index;
  int code;

    /* In ANNOUNCING a PARAM_REQ is answered the way SES-2 answers a
     * TOPIC_REQ, and anything else is ignored like any other non-handshake
     * frame. */
  if (session.state == ANNOUNCING) {
    if (len == 2u && payload[0] == R2S_PARAM_OP_REQ) {
      if (payload[1] < r2s_params_count()) {
        send_declare(payload[1]);
      } else {
        send_plain_status(payload[1], R2S_PARAM_UNKNOWN_INDEX);
      }
    } else {
      answer_with_hello(now);
    }
    return;
  }
    /* The ANNOUNCING branch above returned and those are the only states. A
     * SET carries its opcode, rel_seq and index; anything else on the
     * channel is refused. A value the encoding cannot carry is refused too
     * (PAR-3); a decodable one earns its verdict (PAR-5). */
  if (len < 3u || payload[0] != R2S_PARAM_OP_SET) {
    refuse();
    return;
  }
  rel_seq = payload[1];
  index = payload[2];
  if (index >= r2s_params_count()) {
    code = R2S_PARAM_UNKNOWN_INDEX;
  } else {
    code = r2s_params_check_set(index, payload + 3u, (uint16_t)(len - 3u));
    if (code < 0) {
      refuse();
      return;
    }
  }
    /* Every reliable frame is acked, duplicates included, and the payload
     * applied at most once: only the expected rel_seq applies and advances
     * the counter (REL-3). */
  send_ack(3u, rel_seq);
  if (rel_seq != session.rx_rel_expected) {
    return;
  }
  session.rx_rel_expected++;
  if (code == R2S_PARAM_APPLIED) {
    r2s_params_store(index, payload + 3u, (uint16_t)(len - 3u));
  }
  queue_status(index, (uint8_t)code, now);
}

#endif

#if defined(R2S_WITH_PARAMS) || defined(R2S_WITH_ACTIONS)
static void on_ack(const uint8_t *payload, uint16_t len, uint32_t now)
{
    /* A stale ack in ANNOUNCING is answered like any other non-handshake
     * frame. An ID 6 frame is never acked itself (REL-4). */
  if (session.state == ANNOUNCING) {
    answer_with_hello(now);
    return;
  }
    /* The ANNOUNCING branch above returned and those are the only states. */
  if (len != 2u) {
    return;
  }
#ifdef R2S_WITH_PARAMS
    /* Channel 3 names the reliable exchange the ack answers. */
  if (payload[0] == 3u && session.rel_out_active != 0u && payload[1] == session.rel_out_rel) {
    session.rel_out_active = 0u;
    promote_status(now);
  }
#endif
#ifdef R2S_WITH_ACTIONS
    /* Channel 8 names the actions exchange the ack answers. */
  if (payload[0] == 8u) {
    r2s_actions_on_ack(payload[1], now);
  }
#endif
}
#endif

#ifdef R2S_WITH_PARAMS

/* The 500 ms timer: resends the outstanding STATUS, or drops and counts it
 * after 3 retries (REL-2). A pending STATUS moves up either way, so an
 * applied SET is never left unreported. */
static void params_poll(uint32_t now)
{
  if (session.rel_out_active == 0u ||
    (uint32_t)(now - session.rel_out_deadline) < ms_to_ticks(REL_RETRY_MS))
  {
    return;
  }
  if (session.rel_out_retries >= REL_RETRIES) {
    session.rel_out_active = 0u;
    r2s_framer_count_refused();
    promote_status(now);
    return;
  }
  session.rel_out_retries++;
  send_status_now(session.rel_out_payload, now);
}
#endif

#ifdef R2S_WITH_SERVICES
/* An ack for the MCU-to-host services stream: frees it when it names the
 * outstanding frame, and ignores anything else (REL-2). A stale ack in
 * ANNOUNCING is answered like any other non-handshake frame. */
static void on_service_ack(const uint8_t *payload, uint16_t len, uint32_t now)
{
  if (session.state == ANNOUNCING) {
    answer_with_hello(now);
    return;
  }
  if (len != 2u || payload[0] != 7u) {
    return;
  }
  r2s_services_note_ack(payload[1]);
}

/* The services channel (SVR-7, SVR-9, SVR-4 to SVR-6, REL-3). Walk answers
 * go out in ANNOUNCING; REQUEST and RESPONSE frames ride the reliable
 * channel while connected, acked every time and applied at most once. */
static void on_services(const uint8_t *payload, uint16_t len, uint32_t now)
{
    /* In ANNOUNCING a QUERY is answered the way SES-2 answers a TOPIC_REQ,
     * a STATUS verdict is stored, and anything else is answered like any
     * other non-handshake frame. */
  if (session.state == ANNOUNCING) {
    if (len == 2u && payload[0] == R2S_SVC_OP_QUERY) {
      r2s_services_answer_query(payload[1]);
    } else if (len == 4u && payload[0] == R2S_SVC_OP_STATUS) {
            /* A reserved verdict is refused the way a bad TOPIC_STATUS is;
             * anything else is stored. */
      if (payload[3] > R2S_SVC_STATUS_NAME_CONFLICT) {
        refuse();
      } else {
        r2s_services_note_status(payload[2], payload[3]);
      }
    } else {
      answer_with_hello(now);
    }
    return;
  }
  if (len < 4u) {
    refuse();
    return;
  }
  switch (payload[0]) {
    case R2S_SVC_OP_REQUEST: {
        /* The frame carries rel_seq, req_id and the service index in that
         * order (SVR-1). */
        const r2s_service_t *service = r2s_services_get(payload[3]);

        /* The host calls a service the MCU announced as one it calls
         * itself: refused without an ack, like an undecodable SET. */
        if (service == NULL || service->role != R2S_SVC_SERVER) {
          refuse();
          return;
        }
        send_ack(7u, payload[1]);
        /* The frame was acked above; a full slot is refused and counted
         * after it, since a conforming peer never sends one. */
        if (r2s_services_apply_request(payload[1], payload[3], payload[2], payload + 4u,
          (uint16_t)(len - 4u)) < 0)
        {
          refuse();
        }
        return;
      }
    case R2S_SVC_OP_RESPONSE:
        /* A RESPONSE with a reserved status is refused before its ack. */
      if (payload[3] > R2S_SVC_BUSY) {
        refuse();
        return;
      }
      send_ack(7u, payload[1]);
      r2s_services_apply_response(payload[1], payload[2], payload[3], payload + 4u,
        (uint16_t)(len - 4u));
      return;
    default:
      break;
  }
  refuse();
}
#endif

#ifdef R2S_WITH_TIME
static uint32_t read_u32(const uint8_t *at)
{
  return (uint32_t)at[0] | ((uint32_t)at[1] << 8u) | ((uint32_t)at[2] << 16u) | ((uint32_t)at[3] <<
         24u);
}

static int16_t read_i16(const uint8_t *at)
{
  return (int16_t)((uint16_t)at[0] | ((uint16_t)at[1] << 8u));
}

/* A CORRECTION carries its five fields in this many payload bytes (TIM-10);
 * the drift and the nanosec stay inside the header's limits (TIM-4, TIM-11).
 * Anything else is refused before it can anchor r2s_now. */
#define TIME_CORRECTION_LEN 15u

static void send_pong(uint8_t ping_id, uint32_t tick)
{
  if (r2s_tx_begin(1u, session.tx_seq_time, 6u) != R2S_OK) {
    return;
  }
  put_byte(OP_PONG);
  put_byte(ping_id);
  put_u32(tick);
  if (r2s_tx_end() == R2S_OK) {
    session.tx_seq_time++;
  }
}

static void on_time(uint32_t delimiter_tick, const uint8_t *payload, uint16_t len)
{
  int16_t drift;
  uint32_t nanosec;

  if (len == 0u) {
    refuse();
    return;
  }
  switch (payload[0]) {
    case OP_PING:
      if (len != 2u) {
        refuse();
        return;
      }
      send_pong(payload[1], delimiter_tick);
      break;
    case OP_CORRECTION:
      if (len != TIME_CORRECTION_LEN) {
        refuse();
        return;
      }
      drift = read_i16(payload + 13u);
      nanosec = read_u32(payload + 9u);
      if (drift > R2S_TIME_MAX_DRIFT_PPM || drift < -R2S_TIME_MAX_DRIFT_PPM ||
        nanosec >= R2S_TIME_MAX_NANOSEC)
      {
        refuse();
        return;
      }
      r2s_time_note_correction(read_u32(payload + 1u), (int32_t)read_u32(payload + 5u), nanosec,
      drift);
      break;
    default:
      refuse();
      break;
  }
}
#endif

/* Answers a non-handshake host frame with HELLO, at most once per 100 ms
 * (SES-3). Solicited answers do not go through here and are never limited. */
static void answer_with_hello(uint32_t now)
{
  if ((uint32_t)(now - session.last_nack_tick) >= ms_to_ticks(HELLO_NACK_MS)) {
    session.last_nack_tick = now;
    send_hello();
  }
}

static void on_control(uint32_t now, const uint8_t *payload, uint16_t len)
{
  uint8_t i;

  if (len == 0u) {
    refuse();
    return;
  }
  switch (payload[0]) {
    case OP_HELLO_REQ:
      if (len != 2u) {
        refuse();
        return;
      }
      enter_announcing(now);
      send_hello();
      break;
    case OP_TOPIC_REQ:
      if (len != 2u || payload[1] >= r2s_topic_count()) {
        refuse();
        return;
      }
      send_topic(payload[1]);
      break;
    case OP_TOPIC_STATUS:
      if (len != 3u || payload[2] > 3u) {
        refuse();
        return;
      }
      i = r2s_topic_find(payload[1]);
      if (i == 0xFFu) {
        refuse();
        r2s_error_note(R2S_ERROR_UNKNOWN_TOPIC, payload[1], 0xFFu, 1u);
        break;
      }
      session.status[i] = payload[2];
      return;
    case OP_READY: {
        /* The token lives in RAM from here (SES-10); a zero token or a
         * short frame is refused and changes nothing. T3 builds one host
         * test library with R2S_TEST_INJECT_READY_ZERO_TOKEN, which keeps
         * a zero token, so the model test can show it catches the breach. */
        uint32_t token;

        if (len != 5u) {
          refuse();
          return;
        }
        token = (uint32_t)payload[1] | ((uint32_t)payload[2] << 8u) | ((uint32_t)payload[3] <<
          16u) |
          ((uint32_t)payload[4] << 24u);
#ifndef R2S_TEST_INJECT_READY_ZERO_TOKEN
        if (token == 0u) {
          refuse();
          return;
        }
#endif
        if (session.state == ANNOUNCING) {
          session.token = token;
          enter_connected(now);
        }
        break;
      }
    default:
      refuse();
      break;
  }
}

static void on_frame(const r2s_frame_t *frame)
{
  uint8_t topic = frame->data[0];
  const uint8_t *payload = frame->data + 2u;
  uint16_t payload_len = (uint16_t)(frame->len - 4u);

    /* Every frame the framer accepted proves the host is alive, whichever
     * channel it arrived on and whether the session refuses it. */
  session.last_host_tick = frame->tick;
  if (topic == 0u) {
    on_control(frame->tick, payload, payload_len);
  } else if (topic == 4u) {
    if (payload_len != 0u) {
      refuse();
    } else if (session.state == ANNOUNCING) {
      answer_with_hello(frame->tick);
    }
#ifdef R2S_WITH_TIME
  } else if (topic == 1u) {
        /* While connected a PING is answered and a CORRECTION anchors
         * r2s_now(); in ANNOUNCING both are answered like any other
         * non-handshake frame ([SES-3]). */
    if (session.state == CONNECTED) {
      on_time(frame->tick, payload, payload_len);
    } else {
            /* ANNOUNCING is the only other state. */
      answer_with_hello(frame->tick);
    }
#endif
#ifdef R2S_WITH_PARAMS
  } else if (topic == 3u) {
    on_params(payload, payload_len, frame->tick);
#endif
#if defined(R2S_WITH_PARAMS) || defined(R2S_WITH_SERVICES) || defined(R2S_WITH_ACTIONS)
  } else if (topic == 6u) {
#ifdef R2S_WITH_PARAMS
    on_ack(payload, payload_len, frame->tick);
  #endif
#ifdef R2S_WITH_SERVICES
    on_service_ack(payload, payload_len, frame->tick);
  #endif
#endif
#ifdef R2S_WITH_SERVICES
  } else if (topic == 7u) {
    on_services(payload, payload_len, frame->tick);
#endif
  } else if (topic <= 6u) {
        /* The channels later tickets own (log, error report), and the
         * parameters, services and ack channels in a build without them:
         * accepted, and answered in ANNOUNCING like any other
         * non-handshake frame, otherwise ignored. */
    if (session.state == ANNOUNCING) {
      answer_with_hello(frame->tick);
    }
#ifdef R2S_WITH_ACTIONS
  } else if (topic == 8u) {
        /* The walk answers in ANNOUNCING; any other non-handshake frame is
         * answered like one ([SES-3]). In CONNECTED the actions layer owns
         * the channel. Without the layer the channel stays unowned, so the
         * reserved rule below applies instead. */
    if (session.state == ANNOUNCING &&
      (payload_len == 0u || (payload[0] != R2S_ACT_OP_QUERY && payload[0] != R2S_ACT_OP_STATUS)))
    {
      answer_with_hello(frame->tick);
    } else {
      r2s_actions_on_frame(payload, payload_len, frame->tick, session.state == CONNECTED);
    }
#endif
  } else if (topic >= 16u) {
#ifdef R2S_WITH_CODEC
        /* A configured topic dispatches while connected; in ANNOUNCING the
         * frame is answered like any other non-handshake frame ([SES-3]). */
    if (session.state == CONNECTED) {
      r2s_codec_dispatch(topic, frame->data[1], payload, payload_len);
    } else {
      answer_with_hello(frame->tick);
    }
#else
        /* A user topic has no consumer until the codec lands; it is accepted
         * and ignored, and in ANNOUNCING answered so the host notices. */
    if (session.state == ANNOUNCING) {
      answer_with_hello(frame->tick);
    }
#endif
  } else {
        /* Reserved IDs are ignored without counting and without a reply, ahead
         * of the ANNOUNCING rule (SYS-4 over SES-3). */
  }
  if (session.state == ANNOUNCING) {
    session.heard = 1u;
  }
}

void r2s_init(void)
{
  uint32_t now = r2s_port_tick();

  session.token = 0u;
  session.tx_seq_ctrl = 0u;
  session.tx_seq_hb = 0u;
#ifdef R2S_WITH_TIME
  session.tx_seq_time = 0u;
#endif
  enter_announcing(now);
  send_hello();
}

void r2s_spin_some(void)
{
  uint16_t n = 0u;
  r2s_frame_t frame;
  uint32_t now;
  uint16_t overflow = 0u;
  uint16_t too_large = 0u;
  uint8_t too_large_topic = 0xFFu;

  while (n < R2S_SPIN_MAX_FRAMES && r2s_framer_poll(&frame) != 0u) {
    on_frame(&frame);
    r2s_framer_release();
    n++;
  }
    /* Drops the framer refused outright never reach on_frame, so their
     * reports are taken here: overflows name no topic, while a frame past
     * the limit names the topic it decoded. The flush also releases a
     * report whose rate limit expired with no new failure. */
  r2s_framer_take_error_reports(&overflow, &too_large, &too_large_topic);
  if (overflow != 0u) {
    r2s_error_note(R2S_ERROR_RX_OVERFLOW, 0xFFu, 0xFFu, overflow);
  }
  if (too_large != 0u) {
    r2s_error_note(R2S_ERROR_FRAME_TOO_LARGE, too_large_topic, 0xFFu, too_large);
  }
  r2s_error_flush();
  now = r2s_port_tick();
  if (session.state == CONNECTED &&
    (uint32_t)(now - session.last_host_tick) >= ms_to_ticks(R2S_LINK_TIMEOUT_MS))
  {
    enter_announcing(now);
  }
#ifdef R2S_WITH_PARAMS
    /* The retransmit timer runs before the heartbeat, so a due STATUS goes
     * out ahead of the periodic frame. */
  if (session.state == CONNECTED) {
    params_poll(now);
  }
#endif
#ifdef R2S_WITH_SERVICES
    /* The services timer runs on the same rule, after the parameters one,
     * so the two streams keep their own cadence. */
  if (session.state == CONNECTED) {
    r2s_services_poll(now);
  }
#endif
#ifdef R2S_WITH_ACTIONS
    /* The actions retransmit joins the same slot: a due response goes out
     * ahead of the periodic frame. */
  if (session.state == CONNECTED) {
    r2s_actions_poll(now);
  }
#endif
  if (session.state == ANNOUNCING) {
    if (session.heard == 0u &&
      (uint32_t)(now - session.last_hello_tick) >= ms_to_ticks(HELLO_REPEAT_MS))
    {
      session.last_hello_tick = now;
      send_hello();
    }
  } else if ((uint32_t)(now - session.last_hb_tick) >= ms_to_ticks(R2S_HEARTBEAT_MS)) {
    session.last_hb_tick = now;
    send_heartbeat();
  }
}

uint8_t r2s_connected(void)
{
  return session.state;
}

#ifdef R2S_TESTING
void r2s_session_test_reset(void)
{
  uint8_t i;

  session.state = ANNOUNCING;
  session.node_name = "";
  r2s_topics_test_reset();
  for (i = 0u; i < R2S_SESSION_MAX_TOPICS; i++) {
    session.status[i] = 0xFFu;
    session.tx_seq[i] = 0u;
    session.rx_expected[i] = 0u;
    session.rx_lost[i] = 0u;
  }
  session.tx_seq_ctrl = 0u;
  session.tx_seq_hb = 0u;
#ifdef R2S_WITH_PARAMS
  drop_reliable();
#endif
#ifdef R2S_WITH_SERVICES
  r2s_services_note_reconnect();
#endif
#ifdef R2S_WITH_ACTIONS
#if !defined(R2S_WITH_PARAMS)
  session.tx_seq_ack = 0u;
#endif
  r2s_actions_note_reset();
#endif
#ifdef R2S_WITH_TIME
  session.tx_seq_time = 0u;
#endif
  session.token = 0u;
  session.last_host_tick = 0u;
  session.last_hello_tick = 0u;
  session.last_hb_tick = 0u;
  session.last_nack_tick = 0u;
  session.heard = 0u;
}

void r2s_session_test_link_timeout(void)
{
  enter_announcing(r2s_port_tick());
}
#endif
