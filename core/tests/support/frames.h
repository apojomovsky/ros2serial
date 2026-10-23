/* Frames for the tests that need the wire form of a valid one, or of one
 * whose check fails. */
#ifndef R2S_TEST_FRAMES_H
#define R2S_TEST_FRAMES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes one frame, delimiter included, into out and returns its length:
 * topic_id, seq, the payload, its CRC-16/MCRF4XX and the rCOBS encoding
 * (FRM-1, FRM-2, CHK-3). out must hold the COB-8 bound of 4 + payload_len
 * bytes, plus the delimiter. */
uint16_t r2s_test_frame(uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                        uint16_t payload_len);

/* The same frame with its second CRC byte flipped, so the receiver's check
 * fails (FRM-8). */
uint16_t r2s_test_frame_bad_crc(uint8_t *out, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                                uint16_t payload_len);

#ifdef __cplusplus
}
#endif

#endif
