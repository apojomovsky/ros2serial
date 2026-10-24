/* The transmit path: one frame out through the fake port, its space reserved
 * before the first byte, its bytes handed over in chunks, and a busy port left
 * alone. */
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_tx.h"
#include "unity.h"

/* Feeds one whole frame through the transmit path. */
static void send(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_begin(topic_id, seq, len));
    for (uint16_t i = 0; i < len; i++) {
        TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_byte(payload[i]));
    }
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_end());
}

/* The bytes the port captured must be the frame the frame maker builds, which
 * is the wire form of FRM-2 with the check of CHK-3. */
static void assert_sent(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    static uint8_t expected[R2S_FRAMER_RX_CAPACITY + 1u];
    uint16_t expected_len = r2s_test_frame(expected, topic_id, seq, payload, len);
    uint16_t got_len;
    const uint8_t *got = fake_port_tx(&got_len);

    R2S_ASSERT_BYTES(expected, expected_len, got, got_len, "frame on the wire");
}

void test_tx_a_frame_is_written_in_the_wire_form(void)
{
    const uint8_t payload[] = {0x00, 0x00, 0xc0, 0x3f};

    send(16, 0, payload, sizeof payload);

    assert_sent(16, 0, payload, sizeof payload);
}

void test_tx_an_empty_payload_still_makes_a_frame(void)
{
    send(4, 9, NULL, 0);

    assert_sent(4, 9, NULL, 0);
}

void test_tx_zero_bytes_become_code_bytes(void)
{
    const uint8_t payload[] = {0x00, 0x00, 0x00};

    send(16, 3, payload, sizeof payload);

    assert_sent(16, 3, payload, sizeof payload);
}

/* A group of 254 non-zero bytes ends with its own code byte, so the encoder
 * emits FF and then a trailing empty group (COB-3). */
void test_tx_254_non_zero_bytes_end_in_a_trailing_empty_group(void)
{
    uint8_t payload[254];

    memset(payload, 0x11, sizeof payload);
    send(16, 1, payload, sizeof payload);

    assert_sent(16, 1, payload, sizeof payload);
}

void test_tx_a_busy_port_writes_nothing(void)
{
    const uint8_t payload[] = {0x01, 0x02};
    uint16_t bound = r2s_tx_wire_bound(sizeof payload);

    fake_port_set_tx_free((uint16_t)(bound - 1u));

    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY, r2s_tx_begin(16, 0, sizeof payload));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_len());
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_write_calls());
}

void test_tx_space_for_exactly_the_bound_takes_the_frame(void)
{
    const uint8_t payload[] = {0x11, 0x22, 0x00, 0x33};

    fake_port_set_tx_free(r2s_tx_wire_bound(sizeof payload));

    send(16, 0, payload, sizeof payload);

    assert_sent(16, 0, payload, sizeof payload);
}

void test_tx_a_blocking_port_never_reports_busy(void)
{
    uint8_t payload[300];

    memset(payload, 0x11, sizeof payload);
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, r2s_port_tx_free());

    send(16, 0, payload, sizeof payload);

    assert_sent(16, 0, payload, sizeof payload);
}

void test_tx_every_write_is_at_most_one_chunk(void)
{
    uint8_t payload[40];

    memset(payload, 0x11, sizeof payload);
    send(16, 1, payload, sizeof payload);

    TEST_ASSERT_EQUAL_UINT16(R2S_TX_CHUNK, fake_port_tx_write_max());
    TEST_ASSERT_TRUE(fake_port_tx_write_calls() > 1);
}

void test_tx_a_frame_over_the_limit_is_refused_and_writes_nothing(void)
{
    uint16_t too_long = (uint16_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD + 1u);

    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_tx_begin(16, 0, too_long));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_len());
}

/* covers: FRM-11 */
void test_tx_a_frame_at_the_limit_is_accepted(void)
{
    static uint8_t payload[R2S_MAX_FRAME - R2S_TX_OVERHEAD];

    memset(payload, 0x11, sizeof payload);
    send(16, 0, payload, (uint16_t)sizeof payload);

    assert_sent(16, 0, payload, (uint16_t)sizeof payload);
}

void test_tx_a_byte_beyond_the_declared_payload_is_refused(void)
{
    const uint8_t payload[] = {0x11};

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_begin(16, 0, 1u));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_byte(payload[0]));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_tx_byte(0x22));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_end());

    assert_sent(16, 0, payload, sizeof payload);
}

void test_tx_a_short_payload_is_cut_off_and_refused(void)
{
    r2s_frame_t frame;
    uint16_t len;
    const uint8_t *tx;

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_begin(16, 0, 3u));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_byte(0x11));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_tx_end());

    tx = fake_port_tx(&len);
    TEST_ASSERT_TRUE(len > 0u);
    TEST_ASSERT_EQUAL_HEX8(0x00, tx[len - 1u]);

    /* The receiver drops the cut-off frame and starts the next one after its
     * delimiter, so the following frame is not glued onto it. */
    fake_port_queue_rx(tx, len);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(1, r2s_framer_counters()->dropped_frames);
}

void test_tx_starts_a_frame_only_when_none_is_active(void)
{
    const uint8_t payload[] = {0x33};

    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_tx_byte(0x11));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_tx_end());
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_len());

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_begin(16, 0, 1u));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_tx_begin(16, 1, 1u));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_byte(payload[0]));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_tx_end());

    assert_sent(16, 0, payload, sizeof payload);
}

void test_tx_a_sent_frame_decodes_back_to_its_payload(void)
{
    const uint8_t payload[] = {0xde, 0xad, 0x00, 0xbe, 0xef};
    r2s_frame_t frame;
    uint16_t len;
    const uint8_t *tx;

    send(16, 7, payload, sizeof payload);

    tx = fake_port_tx(&len);
    fake_port_queue_rx(tx, len);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_framer_poll(&frame));
    TEST_ASSERT_EQUAL_UINT16(R2S_TX_OVERHEAD + sizeof payload, frame.len);
    TEST_ASSERT_EQUAL_HEX8(16, frame.data[0]);
    TEST_ASSERT_EQUAL_HEX8(7, frame.data[1]);
    R2S_ASSERT_BYTES(payload, sizeof payload, frame.data + 2, sizeof payload, "payload");
}
