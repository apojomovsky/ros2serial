/* The optional receive ring: bytes in through r2s_rx_push() from the
 * interrupt, out through the r2s_port_read() the core provides. This is the
 * R2S_WITH_RX_RING build's test, so it links no port. */
#include "r2s/r2s.h"
#include "r2s/r2s_port.h"

#include "unity.h"

#define CAPACITY R2S_RX_RING_CAPACITY

void setUp(void)
{
    r2s_rx_test_reset();
}

void tearDown(void)
{
}

static void push_run(uint8_t first, uint16_t count)
{
    for (uint16_t i = 0; i < count; i++) {
        r2s_rx_push((uint8_t)(first + (uint8_t)i));
    }
}

static uint8_t read_one(void)
{
    uint8_t byte = 0;

    TEST_ASSERT_EQUAL_UINT16(1, r2s_port_read(&byte, 1));
    return byte;
}

void test_rx_ring_read_from_an_empty_ring_returns_zero(void)
{
    uint8_t buf[4] = {0xaa, 0xaa, 0xaa, 0xaa};

    TEST_ASSERT_EQUAL_UINT16(0, r2s_port_read(buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(0xaa, buf[0]);
}

void test_rx_ring_pushed_bytes_come_back_in_order(void)
{
    uint8_t buf[3] = {0};

    r2s_rx_push(0x11);
    r2s_rx_push(0x22);
    r2s_rx_push(0x33);

    TEST_ASSERT_EQUAL_UINT16(2, r2s_port_read(buf, 2));
    TEST_ASSERT_EQUAL_HEX8(0x11, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x22, buf[1]);
    TEST_ASSERT_EQUAL_UINT16(1, r2s_port_read(buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(0x33, buf[0]);
}

void test_rx_ring_read_moves_at_most_max_bytes(void)
{
    uint8_t buf[8] = {0};

    push_run(1, 5);

    TEST_ASSERT_EQUAL_UINT16(0, r2s_port_read(buf, 0));
    TEST_ASSERT_EQUAL_UINT16(3, r2s_port_read(buf, 3));
    TEST_ASSERT_EQUAL_HEX8(1, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(2, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(3, buf[2]);
    TEST_ASSERT_EQUAL_UINT16(2, r2s_port_read(buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(4, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(5, buf[1]);
    TEST_ASSERT_EQUAL_UINT16(0, r2s_port_read(buf, sizeof buf));
}

void test_rx_ring_full_ring_holds_capacity_and_drops_the_newest_byte(void)
{
    static uint8_t buf[R2S_RX_RING_SIZE];

    push_run(1, CAPACITY);
    r2s_rx_push(0xaa);

    TEST_ASSERT_EQUAL_UINT16(CAPACITY, r2s_port_read(buf, sizeof buf));
    for (uint16_t i = 0; i < CAPACITY; i++) {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(i + 1u), buf[i]);
    }
    TEST_ASSERT_EQUAL_UINT16(0, r2s_port_read(buf, sizeof buf));
}

void test_rx_ring_drain_after_a_full_ring_accepts_bytes_again(void)
{
    static uint8_t buf[R2S_RX_RING_SIZE];

    push_run(1, CAPACITY);
    TEST_ASSERT_EQUAL_UINT16(CAPACITY, r2s_port_read(buf, sizeof buf));

    r2s_rx_push(0x55);
    TEST_ASSERT_EQUAL_UINT16(1, r2s_port_read(buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(0x55, buf[0]);
}

void test_rx_ring_partial_read_then_push_keeps_the_order(void)
{
    uint8_t buf[3] = {0};

    r2s_rx_push(0x11);
    r2s_rx_push(0x22);
    r2s_rx_push(0x33);
    TEST_ASSERT_EQUAL_HEX8(0x11, read_one());

    r2s_rx_push(0x44);

    TEST_ASSERT_EQUAL_UINT16(3, r2s_port_read(buf, sizeof buf));
    TEST_ASSERT_EQUAL_HEX8(0x22, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x33, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x44, buf[2]);
}

/* The producer runs while the consumer copies, the ring stays full, and the
 * indices wrap several times. A reader that sampled head into a type wider
 * than a byte could see half of an update on an 8-bit target and lose or
 * repeat a byte here. */
void test_rx_ring_producer_and_consumer_interleave_on_a_full_ring(void)
{
    uint16_t pushed = 0;
    uint16_t read = 0;

    push_run(0, CAPACITY);
    pushed = CAPACITY;
    while (pushed < (uint16_t)(4u * CAPACITY)) {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)read, read_one());
        read++;
        r2s_rx_push((uint8_t)pushed);
        pushed++;
    }
    while (read < pushed) {
        TEST_ASSERT_EQUAL_HEX8((uint8_t)read, read_one());
        read++;
    }
    TEST_ASSERT_EQUAL_UINT16(pushed, read);
}

void test_rx_ring_wraparound_keeps_the_byte_order(void)
{
    for (uint16_t i = 0; i < (uint16_t)(4u * R2S_RX_RING_SIZE); i++) {
        r2s_rx_push((uint8_t)i);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)i, read_one());
    }
}
