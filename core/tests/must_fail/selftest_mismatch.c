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

/* Must fail: the vector self-test rejects a record whose bytes disagree with
 * its hex text, as a mistranscribed vector would. */
#include "unity.h"
#include "vectors.h"

void test_must_fail_selftest_rejects_mistranscribed_bytes(void)
{
    static const uint8_t decoded[] = {0x11, 0x23};
    static const uint8_t encoded[] = {0x11, 0x22, 0x03, 0x00};
    const r2s_vec_rcobs_t v = {"must_fail case 0", "COB-1", decoded, 2u, "11 22", encoded, 4u,
                               "11 22 03 00",      NULL};

    r2s_vec_check_rcobs(&v);
}
