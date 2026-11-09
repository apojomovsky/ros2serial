// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/* error: the MCU's error reports on the error report channel (spec/protocol.md,
 * section 7.5, topic ID 5). Notes count error occurrences; flushes send one
 * report frame per code with the count since its last report. */
#ifndef R2S_ERROR_H
#define R2S_ERROR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The codes [ERR-2] names, in payload order. */
#define R2S_ERROR_BOUND 1u
#define R2S_ERROR_UNKNOWN_TOPIC 2u
#define R2S_ERROR_RX_OVERFLOW 3u
#define R2S_ERROR_FRAME_TOO_LARGE 4u
#define R2S_ERROR_NOT_CONNECTED 5u
#define R2S_ERROR_TX_BUSY 6u

/* Counts count more occurrences of code with its topic and field, and tries
 * to send what is due. A code outside 1 to 6 is ignored; the count wraps at
 * 65535 ([ERR-6]) and carries the topic and field of the latest occurrence.
 * A send that finds no transmitter space leaves the count pending. */
void r2s_error_note(uint8_t code, uint8_t topic_id, uint8_t field, uint16_t count);

/* Sends every pending report the rate limits allow: a publish while not
 * connected only in a connection that has not reported one, a busy
 * transmitter at most once per second ([ERR-3]). Anything else sends at
 * once. A frame that finds no space stays pending. */
void r2s_error_flush(void);

/* Resets the per-connection state on every handshake: the report sequence
 * restarts with it ([FRM-4]) and a new connection may report code 5 again.
 * Pending counts carry over, so failures from the previous connection are
 * not lost. */
void r2s_error_note_connected(void);

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_error_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
