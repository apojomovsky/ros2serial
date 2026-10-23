/* r2s_test_reset(): every stateful core module adds its reset and a
 * dirty-reset-check test here when it lands (C5 onwards). */
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
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
