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

/* The ring at its smallest size, two slots and one usable: the capacity the
 * default-size test cannot reach. This build compiles rx.c with
 * R2S_RX_RING_SIZE=2, so every read here holds a single byte by construction. */
#include "r2s/r2s.h"
#include "r2s/r2s_port.h"

#include "unity.h"

void setUp(void)
{
    r2s_rx_test_reset();
}

void tearDown(void)
{
}

void test_rx_ring_min_holds_one_byte(void)
{
    uint8_t buf[2] = {0};

    r2s_rx_push(0x11);

    TEST_ASSERT_EQUAL_UINT16(1, r2s_port_read(buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(0x11, buf[0]);
    TEST_ASSERT_EQUAL_UINT16(0, r2s_port_read(buf, sizeof buf));
}

void test_rx_ring_min_full_ring_drops_the_newest_byte(void)
{
    uint8_t buf[2] = {0};

    r2s_rx_push(0x11);
    r2s_rx_push(0x22);

    TEST_ASSERT_EQUAL_UINT16(1, r2s_port_read(buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(0x11, buf[0]);
}

void test_rx_ring_min_draining_frees_the_slot(void)
{
    uint8_t buf[2] = {0};

    r2s_rx_push(0x11);
    TEST_ASSERT_EQUAL_UINT16(1, r2s_port_read(buf, sizeof buf));

    r2s_rx_push(0x22);
    TEST_ASSERT_EQUAL_UINT16(1, r2s_port_read(buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(0x22, buf[0]);
}

void test_rx_ring_min_wraparound_keeps_the_byte_order(void)
{
    for (uint16_t i = 0; i < 16; i++) {
        uint8_t byte = 0;

        r2s_rx_push((uint8_t)i);
        TEST_ASSERT_EQUAL_UINT16(1, r2s_port_read(&byte, 1));
        TEST_ASSERT_EQUAL_HEX8((uint8_t)i, byte);
    }
}
