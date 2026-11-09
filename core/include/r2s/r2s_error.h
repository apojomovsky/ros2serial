/* error: the MCU's error reports on the error report channel (spec/protocol.md,
 * section 7.5, topic ID 5). Notes count error occurrences; flushes send one
 * report frame per code with the count since its last report. */
#ifndef R2S_ERROR_H
#define R2S_ERROR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The codes [ERR-2] names, in payload order.
/// A bound violation on decode.
#define R2S_ERROR_BOUND 1u
/// An unknown topic ID.
#define R2S_ERROR_UNKNOWN_TOPIC 2u
/// A receive overflow.
#define R2S_ERROR_RX_OVERFLOW 3u
/// A frame too large.
#define R2S_ERROR_FRAME_TOO_LARGE 4u
/// A send while not connected.
#define R2S_ERROR_NOT_CONNECTED 5u
/// A busy transmitter.
#define R2S_ERROR_TX_BUSY 6u

/// Counts more occurrences of code.
/**
 * Tries to send what is due. A code outside 1 to 6 is ignored; the count
 * wraps at 65535 ([ERR-6]) and carries the topic and field of the latest
 * occurrence. A send that finds no transmitter space leaves the count
 * pending.
 *
 * \param[in] code the error code, 1 to 6.
 * \param[in] topic_id the topic the occurrence names.
 * \param[in] field the field the occurrence names.
 * \param[in] count how many more times it occurred.
 */
void r2s_error_note(uint8_t code, uint8_t topic_id, uint8_t field, uint16_t count);

/// Sends every pending report the rate limits allow.
/**
 * A publish while not connected only in a connection that has not reported
 * one, a busy transmitter at most once per second ([ERR-3]). Anything else
 * sends at once. A frame that finds no space stays pending.
 */
void r2s_error_flush(void);

/// Resets the per-connection state on every handshake.
/**
 * The report sequence restarts with it ([FRM-4]) and a new connection may
 * report code 5 again. Pending counts carry over, so failures from the
 * previous connection are not lost.
 */
void r2s_error_note_connected(void);

#ifdef R2S_TESTING
/// Resets the error reports for host test builds.
/**
 * The same reset, as r2s_test_reset() calls it.
 */
void r2s_error_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
