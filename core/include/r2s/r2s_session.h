// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/* session: the MCU side of the handshake and the link (spec/protocol.md,
 * sections 7 and 8). It announces the firmware, answers the host and sends
 * heartbeats; the codec owns user traffic. */
#ifndef R2S_SESSION_H
#define R2S_SESSION_H

#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_topics.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Ticks per second of r2s_port_tick().
/**
 * It is 1000 times a divisor of 10^6, so a tick converts to nanoseconds with
 * integer math alone (CTL-8).
 */
#ifndef R2S_TICK_HZ
#define R2S_TICK_HZ 1000u
#endif

/// Milliseconds between heartbeats in CONNECTED (SES-9).
#ifndef R2S_HEARTBEAT_MS
#define R2S_HEARTBEAT_MS 500u
#endif

/// Milliseconds without a valid host frame before the link times out (SES-8).
#ifndef R2S_LINK_TIMEOUT_MS
#define R2S_LINK_TIMEOUT_MS 2000u
#endif

/// Frames decoded per r2s_spin_some() call.
/**
 * So a flooding host cannot starve the application.
 */
#ifndef R2S_SPIN_MAX_FRAMES
#define R2S_SPIN_MAX_FRAMES 8u
#endif

/// Topics the handshake can announce.
/**
 * The per-topic state arrays below hold this many entries. The generated
 * configuration raises it together with R2S_TOPIC_MAX_COUNT when the firmware
 * needs more.
 */
#ifndef R2S_SESSION_MAX_TOPICS
#define R2S_SESSION_MAX_TOPICS R2S_TOPIC_MAX_COUNT
#endif

/// The record type C8 named r2s_session_topic_t is r2s_topic_t now.
/**
 * The alias keeps that firmware compiling with the same layout.
 */
typedef r2s_topic_t r2s_session_topic_t;

/// Hands the session its node name and topic table.
/**
 * Call once before r2s_init(); a call afterwards changes what later frames
 * announce. The table carries R2S_FLASH; a RAM table configures through
 * r2s_session_configure_ram instead.
 *
 * \param[in] node_name the node name to announce.
 * \param[in] topics the topic table to announce.
 * \param[in] topic_count how many entries topics holds.
 * \return R2S_OK, or R2S_ERR_BOUNDS when the node name is missing or does not
 * fit the hello frame, or when the bind refuses the table, leaving the
 * previous configuration unchanged.
 */
r2s_status_t r2s_session_configure(
  const char *node_name, const r2s_topic_t *topics,
  uint8_t topic_count);

/// Hands the session a RAM topic table.
/**
 * As the runtime registration path fills one. The same gate as
 * r2s_session_configure(), with plain table reads.
 *
 * \param[in] node_name the node name to announce.
 * \param[in] topics the topic table to announce.
 * \param[in] topic_count how many entries topics holds.
 * \return R2S_OK, or R2S_ERR_BOUNDS when the node name is missing or does not
 * fit the hello frame, or when the bind refuses the table, leaving the
 * previous configuration unchanged.
 */
r2s_status_t r2s_session_configure_ram(
  const char *node_name, const r2s_topic_t *topics,
  uint8_t topic_count);

/// The index of the configured topic with this user topic_id.
/**
 * The codec's dispatch and publish wrappers use it to reach the state below.
 *
 * \param[in] topic_id the user topic ID to look up.
 * \return The topic index, or 0xFF when the table names no such topic.
 */
uint8_t r2s_session_find_topic(uint8_t topic_id);

/// The announced CTL-4 flags of the topic at index.
/**
 * The flags are the truth for the wire ([CTL-5]). Flags past the count read
 * as 0.
 *
 * \param[in] index which configured topic to read.
 * \return The topic's flags.
 */
uint8_t r2s_session_topic_flags(uint8_t index);

/// The last TOPIC_STATUS of the topic at index.
/**
 * 0xFF status means the host has not resolved the topic yet.
 *
 * \param[in] index which configured topic to read.
 * \return The topic's status.
 */
uint8_t r2s_session_topic_status(uint8_t index);

/// The token the last READY assigned.
/**
 * 0 before any READY and after every boot: the value the HELLO echoes
 * (SES-10).
 *
 * \return The session token.
 */
uint32_t r2s_session_token(void);

#ifdef R2S_WITH_ACTIONS
/// Sends one ack on the ack channel for this reliable channel and rel_seq.
/**
 * The actions layer answers through here [REL-1], so the single ack frame
 * seq serves every reliable channel.
 *
 * \param[in] channel the reliable channel this clears.
 * \param[in] rel_seq the rel seq this clears.
 */
void r2s_session_send_ack(uint8_t channel, uint8_t rel_seq);
#endif
/// The seq the next publish on the topic at index will carry.

/**
 * Without advancing the topic's counter ([FRM-4], [FRM-5]).
 *
 * \param[in] index which configured topic to read.
 * \return The next sequence number.
 */
uint8_t r2s_session_tx_seq(uint8_t index);

/// Advances the topic's seq past a frame the transmitter accepted.
/**
 * So a frame that found no space keeps the number the next publish retries
 * with.
 *
 * \param[in] index which configured topic to advance.
 */
void r2s_session_advance_tx_seq(uint8_t index);

/// Counts a received frame on the topic at index against its expected seq.
/**
 * A gap is not an error; only the count moves.
 *
 * \param[in] index which configured topic to count on.
 * \param[in] seq the received frame's sequence number.
 * \return The topic's lost frames since boot ([FRM-15]).
 */
uint16_t r2s_session_note_rx_seq(uint8_t index, uint8_t seq);

/// The lost frames of the topic at index since boot.
/**
 * The only observation of the [FRM-15] count, which no heartbeat counter
 * carries.
 *
 * \param[in] index which configured topic to read.
 * \return The topic's lost frames since boot.
 */
uint16_t r2s_session_rx_lost(uint8_t index);

#ifdef R2S_TESTING
/// Resets the session for host test builds.
/**
 * The same reset, as r2s_test_reset() calls it.
 */
void r2s_session_test_reset(void);

/// Runs the link-timeout transition now for host test builds.
/**
 * As if no valid host frame arrived for R2S_LINK_TIMEOUT_MS (SES-7, SES-8).
 */
void r2s_session_test_link_timeout(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
