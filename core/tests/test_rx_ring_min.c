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
