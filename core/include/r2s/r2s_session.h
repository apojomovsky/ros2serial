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

/* Ticks per second of r2s_port_tick(). It is 1000 times a divisor of 10^6, so
 * a tick converts to nanoseconds with integer math alone (CTL-8). */
#ifndef R2S_TICK_HZ
#define R2S_TICK_HZ 1000u
#endif

/* Milliseconds between heartbeats in CONNECTED (SES-9), and without a valid
 * host frame before the link times out (SES-8). */
#ifndef R2S_HEARTBEAT_MS
#define R2S_HEARTBEAT_MS 500u
#endif

#ifndef R2S_LINK_TIMEOUT_MS
#define R2S_LINK_TIMEOUT_MS 2000u
#endif

/* Frames decoded per r2s_spin_some() call, so a flooding host cannot starve
 * the application. */
#ifndef R2S_SPIN_MAX_FRAMES
#define R2S_SPIN_MAX_FRAMES 8u
#endif

/* Topics the handshake can announce: the per-topic state arrays below hold
 * this many entries. The generated configuration raises it together with
 * R2S_TOPIC_MAX_COUNT when the firmware needs more. */
#ifndef R2S_SESSION_MAX_TOPICS
#define R2S_SESSION_MAX_TOPICS R2S_TOPIC_MAX_COUNT
#endif

/* The record type C8 named r2s_session_topic_t is r2s_topic_t now; the alias
 * keeps that firmware compiling with the same layout. */
typedef r2s_topic_t r2s_session_topic_t;

/* Hands the session its node name and topic table. Call once before
 * r2s_init(); a call afterwards changes what later frames announce. The
 * table carries R2S_FLASH; a RAM table configures through
 * r2s_session_configure_ram instead. Returns R2S_OK, or R2S_ERR_BOUNDS when
 * the node name is missing or does not fit the hello frame, or when the bind
 * refuses the table, leaving the previous configuration unchanged. */
r2s_status_t r2s_session_configure(const char *node_name, const r2s_topic_t *topics, uint8_t topic_count);

/* Hands the session a RAM topic table, as the runtime registration path fills
 * one. The same gate as above, with plain table reads. */
r2s_status_t r2s_session_configure_ram(const char *node_name, const r2s_topic_t *topics, uint8_t topic_count);

/* The index of the configured topic with this user topic_id, or 0xFF when the
 * table names no such topic. The codec's dispatch and publish wrappers use it
 * to reach the state below. */
uint8_t r2s_session_find_topic(uint8_t topic_id);

/* The announced CTL-4 flags and the last TOPIC_STATUS of the topic at index:
 * the flags are the truth for the wire ([CTL-5]), and 0xFF status means the
 * host has not resolved the topic yet. Flags past the count read as 0. */
uint8_t r2s_session_topic_flags(uint8_t index);
uint8_t r2s_session_topic_status(uint8_t index);

/* The seq the next publish on the topic at index will carry, without
 * advancing the topic's counter ([FRM-4], [FRM-5]). */
uint8_t r2s_session_tx_seq(uint8_t index);

/* Advances the topic's seq past a frame the transmitter accepted, so a frame
 * that found no space keeps the number the next publish retries with. */
void r2s_session_advance_tx_seq(uint8_t index);

/* Counts a received frame on the topic at index against its expected seq and
 * returns the topic's lost frames since boot ([FRM-15]). A gap is not an
 * error; only the count moves. */
uint16_t r2s_session_note_rx_seq(uint8_t index, uint8_t seq);

/* The lost frames of the topic at index since boot: the only observation of
 * the [FRM-15] count, which no heartbeat counter carries. */
uint16_t r2s_session_rx_lost(uint8_t index);

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_session_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
