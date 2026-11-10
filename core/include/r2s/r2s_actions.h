// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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

/// The most actions one firmware announces.
/**
 * The generated configuration raises it when the firmware needs more.
 */
#ifndef R2S_ACTION_MAX_COUNT
#define R2S_ACTION_MAX_COUNT 4u
#endif

// The opcodes of the actions channel, in payload order [ACT-1].
/// The walk's index query.
#define R2S_ACT_OP_QUERY 0x01u
/// The walk's type declaration answer.
#define R2S_ACT_OP_DECLARE 0x02u
/// The walk's host verdict on one DECLARE.
#define R2S_ACT_OP_STATUS 0x03u
/// A goal for one action.
#define R2S_ACT_OP_GOAL 0x04u
/// The answer to a goal.
#define R2S_ACT_OP_GOAL_RESP 0x05u
/// A cancel for one active goal.
#define R2S_ACT_OP_CANCEL 0x06u
/// The answer to a cancel.
#define R2S_ACT_OP_CANCEL_RESP 0x07u
/// Best-effort feedback while running.
#define R2S_ACT_OP_FEEDBACK 0x08u
/// The terminal result of one goal.
#define R2S_ACT_OP_RESULT 0x09u

// The GOAL_RESP decisions [ACT-4], in payload order.
/// The goal is accepted and starts running.
#define R2S_ACT_ACCEPTED 0u
/// The goal is rejected and the action stays idle.
#define R2S_ACT_REJECTED 1u
/// The action is busy and the host retries later.
#define R2S_ACT_BUSY 2u
/// The goal names no bound action.
#define R2S_ACT_UNKNOWN_ACTION 3u

// The CANCEL_RESP decisions [ACT-5], in payload order.
/// The cancel is accepted and the goal stops.
#define R2S_ACT_CANCEL_ACCEPTED 0u
/// The cancel is rejected and the goal keeps running.
#define R2S_ACT_CANCEL_REJECTED 1u
/// The cancel names no active goal.
#define R2S_ACT_CANCEL_UNKNOWN_GOAL 2u

// The RESULT outcomes [ACT-6], in payload order.
/// The goal ran to its end.
#define R2S_ACT_SUCCEEDED 4u
/// The goal stopped after an accepted cancel.
#define R2S_ACT_CANCELED 5u
/// The goal stopped on its own failure.
#define R2S_ACT_ABORTED 6u

// The DECLARE STATUS verdicts [ACT-7], in payload order.
/// The host takes the declared type.
#define R2S_ACT_STATUS_OK 0u
/// The host does not know the declared type.
#define R2S_ACT_STATUS_UNKNOWN_TYPE 1u
/// The host's hash for the type differs.
#define R2S_ACT_STATUS_HASH_MISMATCH 2u
/// The host knows the action name under another type.
#define R2S_ACT_STATUS_NAME_CONFLICT 3u

/// One action's walk state [ACT-7]: the host's verdict on the DECLARE.
/**
 * 0xFF while the host has not answered yet.
 */
#define R2S_ACT_WALK_UNKNOWN 0xFFu

// One goal's state [ACT-8].
/// IDLE takes a goal.
#define R2S_ACT_IDLE 0u
/// EXECUTING runs the accepted goal.
#define R2S_ACT_EXECUTING 1u
/// CANCELING stops the goal after an accepted cancel.
#define R2S_ACT_CANCELING 2u

/// One announced action.
/**
 * The first 8 bytes of its RIHS01 type hash with its type and action
 * names.
 */
typedef struct
{
  uint8_t type_hash[8];
  const char *type_name;
  const char *action_name;
} r2s_action_t;

#ifdef R2S_WITH_ACTIONS
/// Binds the table the walk answers from.
/**
 * The table outlives the call: the static path passes its constant table
 * in flash. The bind is the single gate: it refuses a missing table, a
 * count above R2S_ACTION_MAX_COUNT, a missing name, a name beyond the
 * varint range, or an entry that does not fit the announce frame, leaving
 * the previous binding unchanged. A RAM table binds through
 * r2s_actions_configure_ram instead: flash and RAM addresses overlap on
 * AVR, so the readers cannot tell them apart and the caller names the
 * table's home.
 *
 * \param[in] actions the table to bind.
 * \param[in] count how many entries actions holds.
 * \return R2S_OK on success, R2S_ERR_BOUNDS on a bad table.
 * \pre The caller keeps actions alive past the call.
 */
r2s_status_t r2s_actions_configure(const r2s_action_t *actions, uint8_t count);

/// Binds a RAM table, as the runtime registration path fills one.
/**
 * The same gate as above, with plain reads.
 *
 * \param[in] actions the table to bind.
 * \param[in] count how many entries actions holds.
 * \return R2S_OK on success, R2S_ERR_BOUNDS on a bad table.
 * \pre The caller keeps actions alive past the call.
 */
r2s_status_t r2s_actions_configure_ram(const r2s_action_t *actions, uint8_t count);

/// The number of bound actions, 0 when no table is bound.
/**
 * \return How many entries the bound table holds.
 */
uint8_t r2s_actions_count(void);

/// The entry at index, or NULL past the count.
/**
 * Only dereference it for a RAM table: a flash entry's bytes live in the
 * flash address space on AVR. Copy a flash entry out first.
 *
 * \param[in] index which entry to read.
 * \return The entry, or NULL past the count.
 */
const r2s_action_t * r2s_actions_get(uint8_t index);

/// A RAM copy of the entry at index, read through the bound table's readers.
/**
 * \param[in] index which entry to copy.
 * \param[out] out where the copy goes.
 * \return R2S_OK, or R2S_ERR_BOUNDS past the count or for a NULL out.
 */
r2s_status_t r2s_actions_copy(uint8_t index, r2s_action_t *out);

/// The DECLARE payload length for the entry at index.
/**
 * 0 when the entry is missing. The walk reserves the frame from this
 * before streaming it.
 *
 * \param[in] index which entry to measure.
 * \return The DECLARE payload length, or 0 past the count.
 */
uint16_t r2s_actions_declare_len(uint8_t index);

/// The goal state of the action at index [ACT-8].
/**
 * R2S_ACT_IDLE past the count.
 *
 * \param[in] index which action to read.
 * \return The goal state.
 */
uint8_t r2s_actions_state(uint8_t index);

/// The goal_seq of the active goal at index.
/**
 * 0 past the count and while IDLE.
 *
 * \param[in] index which action to read.
 * \return The active goal seq.
 */
uint8_t r2s_actions_goal_seq(uint8_t index);

/// The host's verdict on the DECLARE at index [ACT-7].
/**
 * R2S_ACT_WALK_UNKNOWN past the count and while the host has not answered.
 *
 * \param[in] index which action to read.
 * \return The host verdict.
 */
uint8_t r2s_actions_status(uint8_t index);

/// Answers the pending goal at index with this decision [ACT-4].
/**
 * The C++ layer calls it from r2s_action_on_goal. Accepted starts
 * EXECUTING with this goal_seq; any other decision leaves IDLE.
 *
 * \param[in] index which action answers.
 * \param[in] goal_seq the goal this answers.
 * \param[in] decision one of the GOAL_RESP codes.
 * \return R2S_OK, or R2S_ERR_NOT_CONNECTED while announcing [ACT-9],
 * R2S_ERR_TX_BUSY when the reliable slot holds no room, or
 * R2S_ERR_BOUNDS on a bad index, a decision past unknown action, or a
 * goal the state no longer holds.
 */
r2s_status_t r2s_actions_answer_goal(uint8_t index, uint8_t goal_seq, uint8_t decision);

/// Answers the cancel at index with this decision [ACT-5].
/**
 * The C++ layer calls it from r2s_action_on_cancel. Accepted moves
 * EXECUTING to CANCELING. Same errors as the goal answer, with decisions
 * past unknown goal refused.
 *
 * \param[in] index which action answers.
 * \param[in] goal_seq the goal this answers.
 * \param[in] decision one of the CANCEL_RESP codes.
 * \return R2S_OK, or the goal answer's errors.
 */
r2s_status_t r2s_actions_answer_cancel(uint8_t index, uint8_t goal_seq, uint8_t decision);

/// Sends this feedback for the active goal best effort [ACT-3].
/**
 * The payload is the packed Feedback message.
 *
 * \param[in] index which action reports.
 * \param[in] goal_seq the goal this belongs to.
 * \param[in] payload the packed Feedback bytes, NULL only with len 0.
 * \param[in] len how many bytes payload holds.
 * \return R2S_OK, or R2S_ERR_NOT_CONNECTED while announcing [ACT-9],
 * R2S_ERR_TX_BUSY when the link takes no bytes, or R2S_ERR_BOUNDS past
 * the count, on a goal_seq the action does not hold, or outside
 * EXECUTING and CANCELING [ACT-8], sending nothing in each case.
 */
r2s_status_t r2s_actions_feedback(
  uint8_t index, uint8_t goal_seq, const uint8_t *payload,
  uint16_t len);

/// Sends this terminal result for the active goal over the reliable channel.
/**
 * The outcome is one of the RESULT codes and the payload is the packed
 * Result message. The goal returns to IDLE at once [ACT-8].
 *
 * \param[in] index which action reports.
 * \param[in] goal_seq the goal this belongs to.
 * \param[in] outcome one of the terminal RESULT codes.
 * \param[in] payload the packed Result bytes, NULL only with len 0.
 * \param[in] len how many bytes payload holds.
 * \return R2S_OK, or the feedback errors, with outcomes outside the
 * terminal codes refused.
 */
r2s_status_t r2s_actions_result(
  uint8_t index, uint8_t goal_seq, uint8_t outcome, const uint8_t *payload,
  uint16_t len);

/// The C++ layer's goal hook: the core calls it while serving IDLE goals.
/**
 * It answers through r2s_actions_answer_goal. The weak default answers
 * rejected, so a firmware without the C++ layer refuses goal work instead
 * of running none.
 *
 * \param[in] index which action was named.
 * \param[in] goal_seq the goal this carries.
 * \param[in] payload the packed goal bytes.
 * \param[in] len how many bytes payload holds.
 */
void r2s_action_on_goal(uint8_t index, uint8_t goal_seq, const uint8_t *payload, uint16_t len);

/// The C++ layer's cancel hook: the core calls it for matching cancels.
/**
 * It answers through r2s_actions_answer_cancel. The weak default answers
 * rejected.
 *
 * \param[in] index which action was named.
 * \param[in] goal_seq the goal this carries.
 */
void r2s_action_on_cancel(uint8_t index, uint8_t goal_seq);

/// Serves one ID 8 frame: the walk in ANNOUNCING, the goal exchange after.
/**
 * \param[in] payload the frame payload.
 * \param[in] len how many bytes payload holds.
 * \param[in] now the receive tick for deadlines.
 * \param[in] connected nonzero while the session is CONNECTED.
 */
void r2s_actions_on_frame(const uint8_t *payload, uint16_t len, uint32_t now, uint8_t connected);

/// Answers the ack for this channel's rel_seq [REL-1].
/**
 * The session routes ID 6 frames naming channel 8 here.
 *
 * \param[in] rel_seq the rel seq this clears.
 * \param[in] now the receive tick for deadlines.
 */
void r2s_actions_on_ack(uint8_t rel_seq, uint32_t now);

/// Retransmits the outstanding reliable payload while CONNECTED [REL-2].
/**
 * \param[in] now the tick deadlines are measured against.
 */
void r2s_actions_poll(uint32_t now);

/// Drops the reliable state and returns every action to IDLE [REL-5, ACT-9].
/**
 * Clears the walk verdicts too. The session calls it on every return to
 * ANNOUNCING. Entering CONNECTED keeps the walk the handshake resolved.
 */
void r2s_actions_note_reset(void);
#endif

#ifdef R2S_TESTING
/// Host test builds only: the same reset, as r2s_test_reset() calls it.
void r2s_actions_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
