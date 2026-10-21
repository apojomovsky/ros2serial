/* The fake port itself: what the core's tests rely on to drive the core
 * through r2s_port_* without hardware (docs/01-testing.md, section 2.1). */
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "r2s/r2s.h"
#include "r2s/r2s_port.h"
#include "unity.h"

void test_fake_port_queued_rx_comes_out_in_order(void)
{
    uint8_t buf[8];

    fake_port_queue_rx_hex("10 02 00");
    fake_port_queue_rx_hex("05");

    TEST_ASSERT_EQUAL_UINT16(2, r2s_port_read(buf, 2));
    TEST_ASSERT_EQUAL_UINT16(2, r2s_port_read(buf + 2, 8));
    R2S_ASSERT_BYTES((const uint8_t *)"\x10\x02\x00\x05", 4, buf, 4, "rx order");
    TEST_ASSERT_EQUAL_UINT16(0, r2s_port_read(buf, 8));
}

void test_fake_port_reading_one_byte_at_a_time_stops_at_each_delimiter(void)
{
    uint8_t b;
    uint16_t taken = 0;

    fake_port_queue_rx_hex("11 01 00 22 01 00");
    while (r2s_port_read(&b, 1) == 1 && b != 0x00) {
        taken++;
    }

    TEST_ASSERT_EQUAL_UINT16(2, taken);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_rx_pending());
}

void test_fake_port_tx_is_captured_and_split_into_encoded_frames(void)
{
    uint16_t len;
    const uint8_t *frame;

    r2s_port_write((const uint8_t *)"\x11\x01", 2);
    r2s_port_write((const uint8_t *)"\x00\x22", 2);
    r2s_port_write((const uint8_t *)"\x01\x00\x33", 3);

    TEST_ASSERT_EQUAL_UINT16(7, fake_port_tx_len());
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    frame = fake_port_tx_frame(0, &len);
    R2S_ASSERT_BYTES((const uint8_t *)"\x11\x01", 2, frame, len, "frame 0");
    frame = fake_port_tx_frame(1, &len);
    R2S_ASSERT_BYTES((const uint8_t *)"\x22\x01", 2, frame, len, "frame 1");
    TEST_ASSERT_NULL(fake_port_tx_frame(2, &len));
}

void test_fake_port_tick_is_settable_and_wraps(void)
{
    fake_port_set_tick(0xFFFFFFFEu);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFEu, r2s_port_tick());

    fake_port_advance(3);

    TEST_ASSERT_EQUAL_HEX32(1u, r2s_port_tick());
}

void test_fake_port_tx_free_is_settable(void)
{
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, r2s_port_tx_free());

    fake_port_set_tx_free(12);

    TEST_ASSERT_EQUAL_UINT16(12, r2s_port_tx_free());
}

void test_fake_port_a_write_shrinks_tx_free(void)
{
    fake_port_set_tx_free(5);

    r2s_port_write((const uint8_t *)"\x01\x02", 2);

    TEST_ASSERT_EQUAL_UINT16(3, r2s_port_tx_free());
}

void test_fake_port_a_write_beyond_tx_free_is_recorded(void)
{
    fake_port_set_tx_free(1);

    r2s_port_write((const uint8_t *)"\x01\x02", 2);

    TEST_ASSERT_EQUAL_UINT16(1, fake_port_take_overruns());
}

void test_fake_port_boot_id_is_the_chosen_one(void)
{
    fake_port_set_boot_id(0xC0FFEE01u);

    TEST_ASSERT_EQUAL_HEX32(0xC0FFEE01u, r2s_port_boot_id());
}

void test_fake_port_reset_restores_every_control(void)
{
    uint8_t b;

    fake_port_queue_rx_hex("01");
    r2s_port_write(&b, 1);
    fake_port_set_tick(9);
    fake_port_set_tx_free(3);
    fake_port_set_boot_id(7);

    fake_port_reset();

    TEST_ASSERT_EQUAL_UINT16(0, fake_port_rx_pending());
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_len());
    TEST_ASSERT_EQUAL_HEX32(0, r2s_port_tick());
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, r2s_port_tx_free());
    TEST_ASSERT_EQUAL_HEX32(0, r2s_port_boot_id());
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_take_overruns());
}

void test_fake_port_unlimited_tx_free_never_shrinks(void)
{
    uint8_t data[300] = {0};

    r2s_port_write(data, sizeof data);

    TEST_ASSERT_EQUAL_UINT16(0xFFFF, r2s_port_tx_free());
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_take_overruns());
}

void test_fake_port_an_overrun_leaves_no_space(void)
{
    fake_port_set_tx_free(1);

    r2s_port_write((const uint8_t *)"\x01\x02", 2);

    TEST_ASSERT_EQUAL_UINT16(0, r2s_port_tx_free());
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_take_overruns());
}

void test_fake_port_consumed_rx_frees_its_space(void)
{
    uint8_t chunk[1000] = {0x11};
    uint8_t sink[1000];

    for (unsigned i = 0; i < 10u; i++) {
        fake_port_queue_rx(chunk, sizeof chunk);
        TEST_ASSERT_EQUAL_UINT16(sizeof sink, r2s_port_read(sink, sizeof sink));
    }

    TEST_ASSERT_EQUAL_UINT16(0, fake_port_rx_pending());
}

void test_fake_port_tx_clear_starts_a_new_capture(void)
{
    r2s_port_write((const uint8_t *)"\x11\x00", 2);

    fake_port_tx_clear();

    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_len());
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

void test_fake_port_scripts_a_full_exchange_byte_by_byte(void)
{
    const uint8_t expected[] = {0x10, 0x02, 0x01, 0x01, 0xc0, 0x3f, 0x1e, 0x49, 0x05};
    uint8_t frame[sizeof expected + 1u];
    uint16_t n = 0;
    uint16_t len;
    const uint8_t *sent;
    uint8_t b;

    fake_port_queue_rx_hex("10 02 01 01 c0 3f 1e 49 05 00");
    while (n < sizeof frame && r2s_port_read(&b, 1) == 1 && b != 0x00) {
        frame[n++] = b;
    }
    r2s_port_write(frame, n);
    r2s_port_write((const uint8_t *)"\x00", 1);

    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    sent = fake_port_tx_frame(0, &len);
    R2S_ASSERT_BYTES(expected, sizeof expected, sent, len, "echoed frame");
}
