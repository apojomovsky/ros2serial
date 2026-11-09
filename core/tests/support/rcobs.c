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

/* The rCOBS encoder, for the tests that need to build an encoding. */
#include "rcobs.h"

#include "r2s/r2s_rcobs.h"

uint16_t r2s_test_rcobs_encode(const uint8_t *data, uint16_t len, uint8_t *out)
{
    r2s_rcobs_encoder_t enc;
    uint16_t written = 0;

    r2s_rcobs_encoder_init(&enc);
    for (uint16_t i = 0; i < len; i++) {
        written = (uint16_t)(written + r2s_rcobs_encode_byte(&enc, data[i], out + written));
    }
    return (uint16_t)(written + r2s_rcobs_encode_end(&enc, out + written));
}
