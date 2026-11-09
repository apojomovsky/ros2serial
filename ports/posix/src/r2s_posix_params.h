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

/* The virtual MCU's parameter table: a bool led and an i64 rate, announced
 * so the end-to-end suite sets them over the reliable channel. */
#ifndef R2S_POSIX_PARAMS_H
#define R2S_POSIX_PARAMS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hands the core the table below. Returns 0, or -1 when the table is unfit,
 * so the firmware refuses to announce a lie. */
int r2s_posix_params_init(void);

/* Prints each applied value the last report has not covered yet, so the
 * end-to-end suite reads the MCU's live values off stderr. */
void r2s_posix_params_report(void);

#ifdef __cplusplus
}
#endif

#endif
