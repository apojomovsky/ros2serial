// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* rCOBS: the framing that removes every 0x00 from a frame, so a 0x00 can
 * delimit it (spec/protocol.md, section 4). Not part of the firmware API: the
 * core's framing uses it, and so does the bridge, which links rcobs.c for its
 * own framing. */
#ifndef R2S_RCOBS_H
#define R2S_RCOBS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Why a decoder refused an encoding: the rule of spec/protocol.md, section 4,
// that the encoding breaks.
/// The encoding broke no rule.
#define R2S_RCOBS_OK 0
/// A code byte the encoding cannot hold (COB-5).
#define R2S_RCOBS_ERR_CODE (-1)
/// Bytes past the last group of the encoding (COB-6).
#define R2S_RCOBS_ERR_TRAILING (-2)

/// The most bytes one input byte, or the end of an encoding, can produce.
#define R2S_RCOBS_MAX_EMIT 2u

/// The state of a streaming encoder.
/**
 * The run counter of COB-1 and nothing else, so the encoder needs no buffer
 * and no lookahead.
 */
typedef struct
{
  uint8_t run;   ///< The code byte of the current group, 1 to 0xFF.
} r2s_rcobs_encoder_t;

/// Starts an encoding.
/**
 * Start the encoder before the first byte.
 *
 * \param[out] enc the encoder to start.
 */
void r2s_rcobs_encoder_init(r2s_rcobs_encoder_t *enc);

/// Appends one byte to the encoding.
/**
 * A byte of 0x00 ends the current group, and a group of 254 bytes ends
 * itself, so one input byte produces at most two output bytes.
 *
 * \param[in,out] enc the encoding in progress.
 * \param[in] byte the next input byte.
 * \param[out] out where to write the produced bytes.
 * \return How many bytes it produced.
 * \pre out holds R2S_RCOBS_MAX_EMIT bytes.
 */
uint8_t r2s_rcobs_encode_byte(r2s_rcobs_encoder_t *enc, uint8_t byte, uint8_t *out);

/// Ends the encoding and writes the last code byte to out.
/**
 * The delimiter that follows a frame on the wire is the framer's, not the
 * encoder's.
 *
 * \param[in,out] enc the encoding in progress.
 * \param[out] out where to write the last code byte.
 * \return How many bytes it wrote, which is one.
 * \pre out holds R2S_RCOBS_MAX_EMIT bytes.
 */
uint8_t r2s_rcobs_encode_end(r2s_rcobs_encoder_t *enc, uint8_t *out);

/// Decodes an encoded frame in place.
/**
 * buf holds len bytes of the encoding, without its delimiter, and the decoded
 * frame is written over it, right aligned, so it ends where the encoded one
 * ended. An empty buffer decodes to an empty frame; the framer never passes
 * one. The length comes back because a caller cannot work it out on an AVR.
 *
 * \param[in,out] buf the encoding to decode in place.
 * \param[in] len how many bytes buf holds.
 * \param[out] decoded where to write the decoded frame's first byte.
 * \param[out] decoded_len where to write the decoded frame's length.
 * \return R2S_RCOBS_OK, R2S_RCOBS_ERR_TRAILING for COB-6, checked before any
 * code is read, or R2S_RCOBS_ERR_CODE for COB-5.
 */
int8_t r2s_rcobs_decode(uint8_t *buf, uint16_t len, uint8_t **decoded, uint16_t *decoded_len);

#ifdef __cplusplus
}
#endif

#endif
