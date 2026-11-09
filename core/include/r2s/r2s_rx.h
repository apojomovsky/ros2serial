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

/* rx: the optional receive ring. Compiled only with R2S_WITH_RX_RING, for a
 * port whose UART interrupt has no ring of its own: the interrupt hands bytes
 * to r2s_rx_push() and the core provides r2s_port_read()
 * (docs/00-design.md, "Receiving and sending bytes"). */
#ifndef R2S_RX_H
#define R2S_RX_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef R2S_WITH_RX_RING

/* Slots in the ring, a power of two so an index masks instead of dividing.
 * One slot stays empty, so a full ring never looks like an empty one. The
 * default holds the wire form of a default-maximum frame (130 bytes). */
#ifndef R2S_RX_RING_SIZE
#define R2S_RX_RING_SIZE 256
#endif

#if R2S_RX_RING_SIZE < 2 || R2S_RX_RING_SIZE > 256
#error "R2S_RX_RING_SIZE is a slot count from 2 to 256"
#endif
#if (R2S_RX_RING_SIZE & (R2S_RX_RING_SIZE - 1)) != 0
#error "R2S_RX_RING_SIZE is a power of two"
#endif

/* The bytes the ring holds. */
#define R2S_RX_RING_CAPACITY (R2S_RX_RING_SIZE - 1)

/* Runs between the buffer access and the index that publishes it, for a core
 * that reorders memory accesses. Empty on the targets we ship. */
#ifndef R2S_MEMORY_BARRIER
#define R2S_MEMORY_BARRIER() ((void)0)
#endif

/* Adds one received byte. ISR-safe, and never blocks: a byte that arrives on a
 * full ring is dropped, so the framer learns of the loss from a delimiter
 * resync or a bad CRC, which is all a port can report. */
void r2s_rx_push(uint8_t byte);

#endif /* R2S_WITH_RX_RING */

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. Declared
 * without the ring too, where it has no state to clear. */
void r2s_rx_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
