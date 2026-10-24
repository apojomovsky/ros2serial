/* ros2serial core: the public API every firmware includes. */
#ifndef R2S_H
#define R2S_H

#include <stdint.h>

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

#ifdef R2S_TESTING
/* Host test builds only: returns every core module to its state after
 * power-up, so each test starts from the same place. */
void r2s_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
