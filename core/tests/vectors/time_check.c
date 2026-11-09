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

/* Conformance: every time vector through the core's time arithmetic. The
 * check drives the same function r2s_now() uses, with the vector's tick rate,
 * so the C core and the Python reference agree case by case. */
#include <stdio.h>

#include "r2s/r2s_time.h"
#include "unity.h"
#include "vectors.h"

/* covers: TIM-4, TIM-6, TIM-7, TIM-8, TIM-9 */
void r2s_vec_check_time(const r2s_vec_time_t *v)
{
    char label[256];
    r2s_time_t got = {0, 0};
    uint8_t synced;

    snprintf(label, sizeof label, "%s [%s]", v->where, v->covers);
    synced = r2s_time_compute(v->tick_hz, v->tick0, v->sec0, v->nanosec0, v->drift_ppm, v->tick, &got);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(v->synced, synced, label);
    /* Unsynced reads {0, 0}: the check initializes got that way, so a
     * regression that sets a time while unsynced fails here. */
    TEST_ASSERT_EQUAL_INT32_MESSAGE(v->sec, got.sec, label);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(v->nanosec, got.nanosec, label);
}
