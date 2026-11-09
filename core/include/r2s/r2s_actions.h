/* actions: the MCU side of the actions channel (spec/protocol.md, section
 * 7.9). The table lives in flash or RAM with the firmware's actions; the
 * per-action goal state and the ID 8 reliable stream live here. Compiled
 * only with R2S_WITH_ACTIONS. */
#ifndef R2S_ACTIONS_H
#define R2S_ACTIONS_H

#include <stdint.h>

#include "r2s/r2s.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The most actions one firmware announces. The generated configuration
 * raises it when the firmware needs more. */
#ifndef R2S_ACTION_MAX_COUNT
#define R2S_ACTION_MAX_COUNT 4u
#endif

/* The opcodes of the actions channel, in payload order [ACT-1]. */
#define R2S_ACT_OP_QUERY 0x01u
#define R2S_ACT_OP_DECLARE 0x02u
#define R2S_ACT_OP_STATUS 0x03u
#define R2S_ACT_OP_GOAL 0x04u
#define R2S_ACT_OP_GOAL_RESP 0x05u
#define R2S_ACT_OP_CANCEL 0x06u
#define R2S_ACT_OP_CANCEL_RESP 0x07u
#define R2S_ACT_OP_FEEDBACK 0x08u
#define R2S_ACT_OP_RESULT 0x09u

/* The GOAL_RESP decisions [ACT-4], in payload order. */
#define R2S_ACT_ACCEPTED 0u
#define R2S_ACT_REJECTED 1u
#define R2S_ACT_BUSY 2u
#define R2S_ACT_UNKNOWN_ACTION 3u

/* The CANCEL_RESP decisions [ACT-5], in payload order. */
#define R2S_ACT_CANCEL_ACCEPTED 0u
#define R2S_ACT_CANCEL_REJECTED 1u
#define R2S_ACT_CANCEL_UNKNOWN_GOAL 2u

/* The RESULT outcomes [ACT-6], in payload order. */
#define R2S_ACT_SUCCEEDED 4u
#define R2S_ACT_CANCELED 5u
#define R2S_ACT_ABORTED 6u

/* The DECLARE STATUS verdicts [ACT-7], in payload order. */
#define R2S_ACT_STATUS_OK 0u
#define R2S_ACT_STATUS_UNKNOWN_TYPE 1u
#define R2S_ACT_STATUS_HASH_MISMATCH 2u
#define R2S_ACT_STATUS_NAME_CONFLICT 3u

/* One action's walk state [ACT-7]: the host's verdict on the DECLARE, 0xFF
 * while the host has not answered yet. */
#define R2S_ACT_WALK_UNKNOWN 0xFFu

/* One goal's state [ACT-8]: IDLE takes a goal, EXECUTING runs it, CANCELING
 * stops it, and a terminal RESULT returns it to IDLE. */
#define R2S_ACT_IDLE 0u
#define R2S_ACT_EXECUTING 1u
#define R2S_ACT_CANCELING 2u

/* One announced action: the first 8 bytes of its RIHS01 type hash and its
 * type and action names. */
typedef struct
{
  uint8_t type_hash[8];
  const char *type_name;
  const char *action_name;
} r2s_action_t;

#ifdef R2S_WITH_ACTIONS
/* Binds the table the walk answers from. The table outlives the call: the
 * static path passes its constant table in flash. The bind is the single
 * gate: it refuses a missing table, a count above R2S_ACTION_MAX_COUNT, a
 * missing name, a name beyond the varint range, or an entry that does not
 * fit the announce frame, leaving the previous binding unchanged. A RAM
 * table binds through r2s_actions_configure_ram instead: flash and RAM
 * addresses overlap on AVR, so the readers cannot tell them apart and the
 * caller names the table's home. */
r2s_status_t r2s_actions_configure(const r2s_action_t *actions, uint8_t count);

/* Binds a RAM table, as the runtime registration path fills one. The same
 * gate as above, with plain reads. */
r2s_status_t r2s_actions_configure_ram(const r2s_action_t *actions, uint8_t count);

/* The number of bound actions, 0 when no table is bound. */
uint8_t r2s_actions_count(void);

/* The entry at index, or NULL past the count. Only dereference it for a RAM
 * table: a flash entry's bytes live in the flash address space on AVR. Copy
 * a flash entry out first. */
const r2s_action_t * r2s_actions_get(uint8_t index);

/* A RAM copy of the entry at index, read through the bound table's own
 * readers. R2S_OK, or R2S_ERR_BOUNDS past the count or for a NULL out. */
r2s_status_t r2s_actions_copy(uint8_t index, r2s_action_t *out);

/* The DECLARE payload length for the entry at index, or 0 when the entry is
 * missing. The walk reserves the frame from this before streaming it. */
uint16_t r2s_actions_declare_len(uint8_t index);

/* The goal state of the action at index [ACT-8], or R2S_ACT_IDLE past the
 * count. */
uint8_t r2s_actions_state(uint8_t index);

/* The goal_seq of the active goal at index, or 0 past the count and while
 * IDLE. */
uint8_t r2s_actions_goal_seq(uint8_t index);

/* The host's verdict on the DECLARE at index [ACT-7], or R2S_ACT_WALK_UNKNOWN
 * past the count and while the host has not answered. */
uint8_t r2s_actions_status(uint8_t index);

/* Answers the pending goal at index with this decision [ACT-4]: the C++
 * layer calls it from r2s_action_on_goal. Accepted starts EXECUTING with
 * this goal_seq; any other decision leaves IDLE. Returns R2S_OK, or
 * R2S_ERR_NOT_CONNECTED while announcing [ACT-9], R2S_ERR_TX_BUSY when the
 * reliable slot holds no room, or R2S_ERR_BOUNDS on a bad index, a decision
 * past unknown action, or a goal the state no longer holds. */
r2s_status_t r2s_actions_answer_goal(uint8_t index, uint8_t goal_seq, uint8_t decision);

/* Answers the cancel at index with this decision [ACT-5]: the C++ layer
 * calls it from r2s_action_on_cancel. Accepted moves EXECUTING to CANCELING.
 * Same errors as the goal answer, with decisions past unknown goal refused. */
r2s_status_t r2s_actions_answer_cancel(uint8_t index, uint8_t goal_seq, uint8_t decision);

/* Sends this feedback for the active goal best effort [ACT-3]: the payload
 * is the packed Feedback message. Returns R2S_OK, or R2S_ERR_NOT_CONNECTED
 * while announcing [ACT-9], R2S_ERR_TX_BUSY when the link takes no bytes, or
 * R2S_ERR_BOUNDS past the count, on a goal_seq the action does not hold, or
 * outside EXECUTING and CANCELING [ACT-8], sending nothing in each case. */
r2s_status_t r2s_actions_feedback(
  uint8_t index, uint8_t goal_seq, const uint8_t *payload,
  uint16_t len);

/* Sends this terminal result for the active goal over the reliable channel
 * [ACT-3]: the outcome is one of the RESULT codes and the payload is the
 * packed Result message. The goal returns to IDLE at once [ACT-8]. Same
 * errors as feedback, with outcomes outside the terminal codes refused. */
r2s_status_t r2s_actions_result(
  uint8_t index, uint8_t goal_seq, uint8_t outcome, const uint8_t *payload,
  uint16_t len);

/* The C++ layer's goal and cancel hooks: the core calls them while serving
 * IDLE goals and matching cancels, and they answer through the calls above.
 * The weak defaults answer rejected, so a firmware without the C++ layer
 * refuses goal work instead of running none. */
void r2s_action_on_goal(uint8_t index, uint8_t goal_seq, const uint8_t *payload, uint16_t len);
void r2s_action_on_cancel(uint8_t index, uint8_t goal_seq);

/* Serves one ID 8 frame: the walk in ANNOUNCING, the goal exchange in
 * CONNECTED. Connected is nonzero while the session is CONNECTED. */
void r2s_actions_on_frame(const uint8_t *payload, uint16_t len, uint32_t now, uint8_t connected);

/* Answers the ack for this channel's rel_seq [REL-1]: the session routes ID
 * 6 frames naming channel 8 here. */
void r2s_actions_on_ack(uint8_t rel_seq, uint32_t now);

/* Retransmits the outstanding reliable payload while CONNECTED [REL-2]. */
void r2s_actions_poll(uint32_t now);

/* Drops the reliable state, clears the walk verdicts and returns every
 * action to IDLE [REL-5, ACT-9]: the session calls it on every return to
 * ANNOUNCING. Entering CONNECTED keeps the walk the handshake resolved. */
void r2s_actions_note_reset(void);
#endif

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_actions_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
