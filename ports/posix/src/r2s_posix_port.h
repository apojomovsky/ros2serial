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

/* Opening the serial line: the firmware main's handle to the port. */
#ifndef R2S_POSIX_PORT_H
#define R2S_POSIX_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens path as a raw 115200 8-N-1 line with non-blocking reads. Returns the
 * file descriptor the port reads and writes, or -1 with errno set, so the
 * caller reports the path it could not open. */
int r2s_posix_open(const char *path);

/* Skew applied to the tick in parts per million, 0 for a true clock. A
 * positive skew runs the MCU fast, so the host fits a drift it must then
 * correct. Firmware sets it from --tick-skew-ppm before opening the port;
 * values past +-20000 ppm leave the correction range the wire allows. */
void r2s_posix_set_tick_skew_ppm(int32_t ppm);

/* Test-only saturation knob: at most cap bytes per tick leave the port, so
 * a publish past the budget fails with TX_BUSY instead of blocking. 0xFFFF
 * disables the cap, which is the default. The firmware arms it once the
 * link connects, so the handshake always runs uncapped. */
void r2s_posix_set_tx_cap(uint16_t cap);

#ifdef __cplusplus
}
#endif

#endif
