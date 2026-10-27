/* ros2serial core: the public API every firmware includes. */
#ifndef R2S_H
#define R2S_H

#include <stdint.h>

#include "r2s/r2s_rx.h"

/* The wire protocol version (spec/protocol.md, GEN-2). A change on the wire
 * that an older peer cannot handle bumps it. */
#define R2S_PROTOCOL_VERSION 1

/* What every core call returns: 0 for success, a negative error code. */
typedef int8_t r2s_status_t;

#define R2S_OK 0
#define R2S_ERR_NOT_CONNECTED (-1)
#define R2S_ERR_BOUNDS (-2)
#define R2S_ERR_TX_BUSY (-3) /* not enough TX space; nothing was written */
#define R2S_ERR_TOPIC_REJECTED (-4)

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the session: reads the boot ID once, sends HELLO and enters
 * ANNOUNCING (SES-1). The table set by r2s_session_configure() stays; without
 * one the node announces itself nameless with no topics. */
void r2s_init(void);

/* Pulls bytes, answers the host, sends heartbeats and watches the link: up to
 * R2S_SPIN_MAX_FRAMES frames per call, then the timers. */
void r2s_spin_some(void);

/* Nonzero while the handshake completed and user traffic may flow. */
uint8_t r2s_connected(void);

#ifdef R2S_TESTING
/* Host test builds only: returns every core module to its state after
 * power-up, so each test starts from the same place. */
void r2s_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
