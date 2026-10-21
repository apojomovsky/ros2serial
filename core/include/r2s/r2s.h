/* ros2serial core: the public API every firmware includes. */
#ifndef R2S_H
#define R2S_H

/* The wire protocol version (spec/protocol.md, GEN-2). A change on the wire
 * that an older peer cannot handle bumps it. */
#define R2S_PROTOCOL_VERSION 1

#ifdef R2S_TESTING
/* Host test builds only: returns every core module to its state after
 * power-up, so each test starts from the same place. */
void r2s_test_reset(void);
#endif

#endif
