/* session: the MCU side of the handshake and the link (spec/protocol.md,
 * sections 7 and 8). It announces the firmware, answers the host and sends
 * heartbeats; the codec owns user traffic. */
#ifndef R2S_SESSION_H
#define R2S_SESSION_H

#include <stdint.h>

#include "r2s/r2s.h"

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

/* Topics the handshake can announce. The generated configuration raises it
 * when the firmware needs more. */
#ifndef R2S_SESSION_MAX_TOPICS
#define R2S_SESSION_MAX_TOPICS 16u
#endif

/* One announced topic: its user topic_id, its CTL-4 flags, the first 8 bytes
 * of its RIHS01 type hash, and its type and topic names. */
typedef struct {
    uint8_t topic_id;
    uint8_t flags;
    uint8_t type_hash[8];
    const char *type_name;
    const char *topic_name;
} r2s_session_topic_t;

/* Hands the session its node name and topic table. Call once before
 * r2s_init(); a call afterwards changes what later frames announce. Returns
 * R2S_OK, or R2S_ERR_BOUNDS when the table is larger than
 * R2S_SESSION_MAX_TOPICS, a name is missing, or a name does not fit a varint,
 * leaving the previous configuration unchanged. */
r2s_status_t r2s_session_configure(const char *node_name, const r2s_session_topic_t *topics,
                                   uint8_t topic_count);

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_session_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
