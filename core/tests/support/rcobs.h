/* The rCOBS encoder, for the tests that need to build an encoding. */
#ifndef R2S_TEST_RCOBS_H
#define R2S_TEST_RCOBS_H

#include <stdint.h>

/* Encodes len bytes of data into out and returns the length of the
 * encoding, which is at most len + len / 254 + 1 (COB-8). out must hold
 * that many bytes. */
uint16_t r2s_test_rcobs_encode(const uint8_t *data, uint16_t len, uint8_t *out);

#endif
