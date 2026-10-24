/* r2s_test_reset(): every stateful core module adds its reset and a
 * dirty-reset-check test here when it lands (C5 onwards). */
#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_tx.h"
#include "unity.h"

void test_test_reset_is_callable_in_test_builds(void)
{
    r2s_test_reset();
}

void test_test_reset_clears_the_framer(void)
{
    uint8_t wire[R2S_FRAMER_RX_CAPACITY + 1u];
    uint8_t stray = 0xaa;
    r2s_frame_t frame;
    uint16_t len;

    fake_port_queue_rx_hex("01 00");
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, r2s_framer_counters()->dropped_frames);

    /* A byte of a frame in progress must not survive the reset: this frame
     * only decodes and checks out if it starts at its first byte. */
    fake_port_queue_rx(&stray, 1);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));

    r2s_test_reset();

    TEST_ASSERT_EQUAL_UINT16(0, r2s_framer_counters()->rx_frames);
    TEST_ASSERT_EQUAL_UINT16(0, r2s_framer_counters()->crc_errors);
    TEST_ASSERT_EQUAL_UINT16(0, r2s_framer_counters()->rx_overflows);
    TEST_ASSERT_EQUAL_UINT16(0, r2s_framer_counters()->dropped_frames);

    len = r2s_test_frame(wire, 4, 0, NULL, 0);
    fake_port_queue_rx(wire, len);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, r2s_framer_counters()->rx_frames);
}

void test_test_reset_clears_the_transmitter(void)
{
    const uint8_t payload[] = {0x22};
    static uint8_t expected[R2S_FRAMER_RX_CAPACITY + 1u];
    uint16_t expected_len;
    uint16_t len;
    const uint8_t *sent;

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_begin(16, 0, 3u));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_byte(0x11));

    r2s_test_reset();
    fake_port_tx_clear();

    /* The half-written frame must not survive the reset: the next frame is
     * what its own topic, sequence and payload make, with nothing before it. */
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_begin(16, 0, 1u));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_byte(payload[0]));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_end());

    expected_len = r2s_test_frame(expected, 16, 0, payload, sizeof payload);
    sent = fake_port_tx(&len);
    R2S_ASSERT_BYTES(expected, expected_len, sent, len, "frame after the reset");
}
