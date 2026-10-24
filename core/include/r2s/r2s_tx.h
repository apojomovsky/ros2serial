/* tx: payload bytes in, one frame out through the port. The session and the
 * codec feed it a byte at a time, so no buffer holds a frame: the CRC and the
 * rCOBS encoder run as the bytes pass, and at most R2S_TX_CHUNK of them wait
 * for the port (spec/protocol.md, sections 2 to 4; docs/00-design.md,
 * "Receiving and sending bytes"). */
#ifndef R2S_TX_H
#define R2S_TX_H

#include <stdint.h>

#include "r2s/r2s.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The bytes a frame adds around its payload: topic_id, seq and crc16 (FRM-2). */
#define R2S_TX_OVERHEAD 4u

/* How many encoded bytes wait for the port before a write. Every write is at
 * most this long, so a slow link is served in small pieces and the frame never
 * needs a buffer of its own. */
#ifndef R2S_TX_CHUNK
#define R2S_TX_CHUNK 8
#endif

#if R2S_TX_CHUNK < 1 || R2S_TX_CHUNK > 255
#error "R2S_TX_CHUNK is a byte count from 1 to 255"
#endif

/* Returns the most bytes the wire form of a frame with payload_len payload
 * bytes takes, delimiter included: the [COB-8] bound of the decoded frame plus
 * the delimiter. r2s_tx_begin() reserves this many from the port before it
 * writes anything. payload_len is at most R2S_MAX_FRAME - R2S_TX_OVERHEAD,
 * which is what r2s_tx_begin() accepts. */
uint16_t r2s_tx_wire_bound(uint16_t payload_len);

/* Starts a frame on topic_id with sequence seq, whose payload is payload_len
 * bytes. It reserves the whole frame's space from the port first and writes
 * nothing unless it gets all of it, so a publish never blocks and never leaves
 * half a frame on the wire. Returns R2S_OK; R2S_ERR_TX_BUSY when the port
 * cannot take the frame now, so the caller may try later; or R2S_ERR_BOUNDS,
 * writing nothing, when the frame would be longer than R2S_MAX_FRAME (FRM-11)
 * or one is already active. */
r2s_status_t r2s_tx_begin(uint8_t topic_id, uint8_t seq, uint16_t payload_len);

/* Adds one payload byte to the active frame, in order. Returns R2S_OK, or
 * R2S_ERR_BOUNDS and writes nothing when there is no active frame or its
 * payload already holds the payload_len bytes r2s_tx_begin() declared. */
r2s_status_t r2s_tx_byte(uint8_t byte);

/* Ends the active frame: encodes the CRC, ends the rCOBS encoding, writes the
 * delimiter and flushes the bytes the chunk still holds. Returns R2S_OK, or
 * R2S_ERR_BOUNDS when there is no active frame or fewer payload bytes were fed
 * than declared. A short frame is cut off at a delimiter, so the receiver
 * drops it instead of gluing the next frame onto its bytes. */
r2s_status_t r2s_tx_end(void);

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_tx_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
