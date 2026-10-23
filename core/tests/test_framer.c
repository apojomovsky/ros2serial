/* The framer: frames cut on 0x00, decoded in place, checked and counted, and
 * never overwritten while a consumer still holds one. */
#include <string.h>

#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_framer.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

static uint8_t wire[WIRE_MAX];
static uint8_t other[WIRE_MAX];

static const r2s_framer_counters_t *counters(void)
{
    return r2s_framer_counters();
}

/* covers: FRM-13 */
void test_framer_a_frame_is_delivered_decoded(void)
{
    const uint8_t payload[] = {0x00, 0x00, 0xc0, 0x3f};
    r2s_frame_t frame;
    uint16_t len = r2s_test_frame(wire, 16, 0, payload, sizeof payload);

    fake_port_set_tick(77);
    fake_port_queue_rx(wire, len);

    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(8, frame.len);
    TEST_ASSERT_EQUAL_HEX8(16, frame.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0, frame.data[1]);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(payload, frame.data + 2, sizeof payload);
    TEST_ASSERT_EQUAL_HEX16(0, r2s_crc16_update_buf(R2S_CRC16_INIT, frame.data, frame.len));
    TEST_ASSERT_EQUAL_HEX32(77, frame.tick);
}

/* covers: FRM-13 */
void test_framer_a_partial_frame_waits_for_its_delimiter_and_records_its_tick(void)
{
    const uint8_t payload[] = {0x01, 0x2a};
    r2s_frame_t frame;
    uint16_t len = r2s_test_frame(wire, 1, 0, payload, sizeof payload);

    fake_port_queue_rx(wire, (uint16_t)(len - 1u));
    fake_port_set_tick(50);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));

    fake_port_queue_rx(&wire[len - 1u], 1);
    fake_port_set_tick(60);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_HEX32(60, frame.tick);
}

/* covers: FRM-6, FRM-13 */
void test_framer_resyncs_at_the_next_zero_after_garbage(void)
{
    const uint8_t payload[] = {0x01};
    r2s_frame_t frame;
    uint16_t len;

    fake_port_queue_rx_hex("de ad be ef 00");
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, counters()->dropped_frames);

    len = r2s_test_frame(wire, 16, 0, payload, sizeof payload);
    fake_port_queue_rx(wire, len);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_HEX8(16, frame.data[0]);
    TEST_ASSERT_EQUAL_UINT16(1, counters()->rx_frames);
}

/* covers: FRM-14 */
void test_framer_ignores_empty_frames_without_counting_them(void)
{
    const uint8_t payload[] = {0x01};
    r2s_frame_t frame;
    uint16_t len = r2s_test_frame(wire, 16, 0, payload, sizeof payload);

    fake_port_queue_rx_hex("00 00 00");
    fake_port_queue_rx(wire, len);

    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(0, counters()->dropped_frames);
    TEST_ASSERT_EQUAL_UINT16(1, counters()->rx_frames);
}

/* covers: FRM-7 */
void test_framer_drops_a_frame_shorter_than_four_bytes(void)
{
    r2s_frame_t frame;

    fake_port_queue_rx_hex("01 00");
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, counters()->dropped_frames);

    fake_port_queue_rx_hex("01 02 03 04 00");
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(2, counters()->dropped_frames);
    TEST_ASSERT_EQUAL_UINT16(0, counters()->rx_frames);
}

/* covers: FRM-6 */
void test_framer_drops_an_encoding_that_rCOBS_refuses(void)
{
    r2s_frame_t frame;

    fake_port_queue_rx_hex("05 00");
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));

    fake_port_queue_rx_hex("11 22 ff 00");
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));

    TEST_ASSERT_EQUAL_UINT16(2, counters()->dropped_frames);
    TEST_ASSERT_EQUAL_UINT16(0, counters()->crc_errors);
}

/* covers: FRM-8, HB-5 */
void test_framer_drops_a_frame_whose_crc_fails(void)
{
    const uint8_t payload[] = {0x01, 0x02, 0x03};
    r2s_frame_t frame;
    uint16_t len = r2s_test_frame_bad_crc(wire, 16, 0, payload, sizeof payload);

    fake_port_queue_rx(wire, len);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, counters()->crc_errors);
    TEST_ASSERT_EQUAL_UINT16(1, counters()->dropped_frames);
    TEST_ASSERT_EQUAL_UINT16(0, counters()->rx_frames);
}

/* covers: FRM-12, HB-6 */
void test_framer_drops_an_encoded_frame_over_the_buffer(void)
{
    const uint8_t payload[] = {0x01};
    static uint8_t stream[R2S_FRAMER_RX_CAPACITY + 2u];
    r2s_frame_t frame;
    uint16_t len;

    memset(stream, 0xaa, sizeof stream);
    stream[sizeof stream - 1u] = 0x00u;
    fake_port_queue_rx(stream, sizeof stream);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, counters()->rx_overflows);
    TEST_ASSERT_EQUAL_UINT16(1, counters()->dropped_frames);

    len = r2s_test_frame(wire, 16, 0, payload, sizeof payload);
    fake_port_queue_rx(wire, len);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, counters()->rx_frames);
}

/* covers: FRM-12, HB-4 */
void test_framer_accepts_a_frame_exactly_at_the_buffer_bound(void)
{
    static uint8_t payload[R2S_MAX_FRAME];
    r2s_frame_t frame;
    uint16_t len;

    memset(payload, 0xaa, sizeof payload);
    len = r2s_test_frame(wire, 16, 0, payload, (uint16_t)(R2S_MAX_FRAME - 4u));
    TEST_ASSERT_EQUAL_UINT16(WIRE_MAX, len);

    fake_port_queue_rx(wire, len);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(R2S_MAX_FRAME, frame.len);
    TEST_ASSERT_EQUAL_UINT16(1, counters()->rx_frames);
    TEST_ASSERT_EQUAL_UINT16(0, counters()->dropped_frames);
}

/* covers: FRM-12 */
void test_framer_drops_a_decoded_frame_over_the_limit(void)
{
    static uint8_t payload[R2S_MAX_FRAME];
    r2s_frame_t frame;
    uint16_t len;

    /* All zeros encode to one byte more than the decoded length, so the
     * frame fits the buffer while decoding to one byte over the limit. */
    memset(payload, 0, sizeof payload);
    len = r2s_test_frame(wire, 16, 0, payload, (uint16_t)(R2S_MAX_FRAME - 3u));
    TEST_ASSERT_LESS_OR_EQUAL_UINT16(WIRE_MAX, len);

    fake_port_queue_rx(wire, len);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, counters()->dropped_frames);
    TEST_ASSERT_EQUAL_UINT16(0, counters()->rx_overflows);
    TEST_ASSERT_EQUAL_UINT16(0, counters()->rx_frames);
}

/* covers: HB-4 */
void test_framer_delivers_back_to_back_frames_in_order(void)
{
    const uint8_t payload[] = {0x01};
    r2s_frame_t frame;
    uint16_t len = r2s_test_frame(wire, 16, 0, payload, sizeof payload);

    fake_port_queue_rx(wire, len);
    len = r2s_test_frame(other, 17, 0, payload, sizeof payload);
    fake_port_queue_rx(other, len);

    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_HEX8(16, frame.data[0]);
    r2s_framer_release();
    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_HEX8(17, frame.data[0]);
    r2s_framer_release();
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(2, counters()->rx_frames);
}

/* covers: FRM-13 */
void test_framer_a_ready_frame_is_not_overwritten(void)
{
    const uint8_t payload[] = {0x01};
    r2s_frame_t frame;
    uint16_t first_len = r2s_test_frame(wire, 16, 0, payload, sizeof payload);
    uint16_t second_len = r2s_test_frame(other, 17, 0, payload, sizeof payload);

    fake_port_queue_rx(wire, first_len);
    fake_port_queue_rx(other, second_len);

    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_HEX8(16, frame.data[0]);
    TEST_ASSERT_EQUAL_UINT16(second_len, fake_port_rx_pending());

    /* The same frame comes back and no byte of the next one is read. */
    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_HEX8(16, frame.data[0]);
    TEST_ASSERT_EQUAL_UINT16(second_len, fake_port_rx_pending());

    r2s_framer_release();
    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_HEX8(17, frame.data[0]);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_rx_pending());
}

/* covers: FRM-6, FRM-7, FRM-8, FRM-12, HB-4, HB-5, HB-6, HB-7 */
void test_framer_dropped_frames_counts_every_drop(void)
{
    const uint8_t payload[] = {0x01};
    static uint8_t stream[R2S_FRAMER_RX_CAPACITY + 2u];
    r2s_frame_t frame;
    uint16_t len;

    fake_port_queue_rx_hex("01 00");
    fake_port_queue_rx_hex("11 22 ff 00");
    len = r2s_test_frame_bad_crc(wire, 16, 0, payload, sizeof payload);
    fake_port_queue_rx(wire, len);
    memset(stream, 0xaa, sizeof stream);
    stream[sizeof stream - 1u] = 0x00u;
    fake_port_queue_rx(stream, sizeof stream);
    len = r2s_test_frame(wire, 16, 0, payload, sizeof payload);
    fake_port_queue_rx(wire, len);

    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, counters()->rx_frames);
    TEST_ASSERT_EQUAL_UINT16(1, counters()->crc_errors);
    TEST_ASSERT_EQUAL_UINT16(1, counters()->rx_overflows);
    TEST_ASSERT_EQUAL_UINT16(4, counters()->dropped_frames);
}
