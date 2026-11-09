/* The frame check, CRC-16/MCRF4XX (spec/protocol.md, section 3). Not part of
 * the firmware API: the core's framing uses it, and so does the bridge, which
 * links crc.c for its own framing. */
#ifndef R2S_CRC_H
#define R2S_CRC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The value a CRC starts from before the first byte (CHK-1).
#define R2S_CRC16_INIT 0xFFFFu

/// Feeds one more byte into a CRC.
/**
 * Feeding a frame's bytes one at a time from R2S_CRC16_INIT gives its frame
 * check; feeding the check too, little-endian, gives 0 (CHK-4).
 *
 * \param[in] crc the CRC so far.
 * \param[in] byte the next byte to feed.
 * \return The CRC after feeding byte to crc.
 */
uint16_t r2s_crc16_update(uint16_t crc, uint8_t byte);

/// Feeds len bytes of data into a CRC.
/**
 * The same as calling r2s_crc16_update() once per byte. data may be NULL
 * when len is 0.
 *
 * \param[in] crc the CRC so far.
 * \param[in] data the bytes to feed.
 * \param[in] len how many bytes data holds.
 * \return The CRC after feeding the bytes to crc.
 */
uint16_t r2s_crc16_update_buf(uint16_t crc, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
