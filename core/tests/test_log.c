/* The logging calls through the fake port: every level and value type sends
 * one frame on the log channel, and busy or oversize lines send nothing. */
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_arch.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_varint.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

static uint8_t wire[WIRE_MAX];

/* Checks the index-th captured frame against the wire form of a log frame:
 * topic 2, the seq, and the payload of level, text, kind and value bytes. */
static void assert_log(uint16_t index, uint8_t seq, uint8_t level, const char *text, uint8_t kind,
                       const uint8_t *value, uint8_t value_len)
{
    static uint8_t expected[WIRE_MAX];
    static uint8_t payload[64];
    char label[64];
    uint16_t text_len = (uint16_t)strlen(text);
    uint16_t n = 0u;
    uint16_t encoded_len;
    const uint8_t *encoded;
    uint16_t expected_len;

    payload[n++] = level;
    n += r2s_varint_encode(text_len, payload + n);
    memcpy(payload + n, text, text_len);
    n = (uint16_t)(n + text_len);
    payload[n++] = kind;
    if (value_len > 0u) {
        memcpy(payload + n, value, value_len);
        n = (uint16_t)(n + value_len);
    }
    expected_len = r2s_test_frame(expected, 2u, seq, payload, n);
    snprintf(label, sizeof label, "log frame %u", index);
    encoded = fake_port_tx_frame(index, &encoded_len);
    TEST_ASSERT_NOT_NULL_MESSAGE(encoded, label);
    /* The captured frame stops at its delimiter, which the built wire keeps. */
    R2S_ASSERT_BYTES(expected, (uint16_t)(expected_len - 1u), encoded, encoded_len, label);
}

void test_log_info_macro_sends_text_frame(void)
{
    R2S_LOG_INFO("battery low");
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_log(0, 0, 20u, "battery low", 0u, NULL, 0u);
}

void test_log_levels_send_their_severity(void)
{
    static const uint8_t levels[] = {10u, 20u, 30u, 40u, 50u};

    r2s_log_debug(R2S_FSTR("d"));
    r2s_log_info(R2S_FSTR("i"));
    r2s_log_warn(R2S_FSTR("w"));
    r2s_log_error(R2S_FSTR("e"));
    r2s_log_fatal(R2S_FSTR("f"));
    TEST_ASSERT_EQUAL_UINT16(5, fake_port_tx_frame_count());
    assert_log(0, 0, levels[0], "d", 0u, NULL, 0u);
    assert_log(1, 1, levels[1], "i", 0u, NULL, 0u);
    assert_log(2, 2, levels[2], "w", 0u, NULL, 0u);
    assert_log(3, 3, levels[3], "e", 0u, NULL, 0u);
    assert_log(4, 4, levels[4], "f", 0u, NULL, 0u);
}

void test_log_value_types_encode_little_endian(void)
{
    static const uint8_t u16_value[] = {0x34u, 0x12u};
    static const uint8_t i16_value[] = {0xFEu, 0xFFu};
    static const uint8_t u32_value[] = {0x78u, 0x56u, 0x34u, 0x12u};
    static const uint8_t i32_value[] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
    static const uint8_t f32_value[] = {0x00u, 0x00u, 0xC0u, 0x3Fu};

    r2s_log_info_u16(R2S_FSTR("u"), 0x1234u);
    r2s_log_info_i16(R2S_FSTR("i"), -2);
    r2s_log_debug_u32(R2S_FSTR("u"), 0x12345678u);
    r2s_log_debug_i32(R2S_FSTR("i"), -1);
    r2s_log_warn_f32(R2S_FSTR("f"), 1.5f);
    TEST_ASSERT_EQUAL_UINT16(5, fake_port_tx_frame_count());
    assert_log(0, 0, 20u, "u", 1u, u16_value, sizeof u16_value);
    assert_log(1, 1, 20u, "i", 2u, i16_value, sizeof i16_value);
    assert_log(2, 2, 10u, "u", 3u, u32_value, sizeof u32_value);
    assert_log(3, 3, 10u, "i", 4u, i32_value, sizeof i32_value);
    assert_log(4, 4, 30u, "f", 5u, f32_value, sizeof f32_value);
}

void test_log_macro_u16_wraps_the_literal(void)
{
    static const uint8_t value[] = {0xEFu, 0xBEu};

    R2S_LOG_WARN_U16("adc=", 0xBEEFu);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_log(0, 0, 30u, "adc=", 1u, value, sizeof value);
}

void test_log_ram_sends_runtime_text(void)
{
    static const char text[] = "runtime";

    r2s_log_error_ram(text);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_log(0, 0, 40u, "runtime", 0u, NULL, 0u);
}

void test_log_busy_transmitter_sends_nothing(void)
{
    fake_port_set_tx_free(0u);
    R2S_LOG_INFO("dropped");
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
    R2S_LOG_INFO("kept");
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_log(0, 0, 20u, "kept", 0u, NULL, 0u);
}

void test_log_text_past_the_frame_limit_sends_nothing(void)
{
    static char big[600];

    memset(big, 'x', sizeof big - 1u);
    big[sizeof big - 1u] = '\0';
    r2s_log_info(R2S_FSTR(big));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    R2S_LOG_INFO("kept");
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_log(0, 0, 20u, "kept", 0u, NULL, 0u);
}

void test_log_seq_increments_per_frame(void)
{
    R2S_LOG_INFO("a");
    R2S_LOG_INFO("b");
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_log(0, 0, 20u, "a", 0u, NULL, 0u);
    assert_log(1, 1, 20u, "b", 0u, NULL, 0u);
}

/* covers: FRM-4, FRM-5 */
void test_log_seq_restarts_after_a_handshake(void)
{
    const uint8_t ready[] = {0x06u, 0x01u, 0x00u, 0x00u, 0x00u};
    uint16_t wire_len;

    R2S_LOG_INFO("before");
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_log(0, 0, 20u, "before", 0u, NULL, 0u);
    wire_len = r2s_test_frame(wire, 0u, 0u, ready, sizeof ready);
    fake_port_queue_rx(wire, wire_len);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    R2S_LOG_INFO("after");
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_log(1, 0, 20u, "after", 0u, NULL, 0u);
}

void test_log_flash_macros_read_placed_tables(void)
{
    static const uint8_t R2S_FLASH table[] = {0x05u, 0xABu, 0xCDu};

    uint8_t i;

    for (i = 0u; i < 3u; i++) {
        TEST_ASSERT_EQUAL_UINT8(table[i], R2S_FLASH_READ_BYTE(table + i));
    }
    TEST_ASSERT_EQUAL_UINT8(5u, R2S_FLASH_READ_BYTE(table));
}

/* covers: LOG-1, LOG-2, LOG-3 */
void test_log_every_level_covers_every_value_type(void)
{
    static const uint8_t one_u16[] = {0x01u, 0x00u};
    static const uint8_t neg_i16[] = {0xFFu, 0xFFu};
    static const uint8_t half_f32[] = {0x00u, 0x00u, 0x00u, 0x3Fu};
    static const uint8_t one_u32[] = {0x01u, 0x00u, 0x00u, 0x00u};
    static const uint8_t neg_i32[] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};

    r2s_log_debug_u16(R2S_FSTR("u"), 1u);
    r2s_log_debug_i16(R2S_FSTR("i"), -1);
    r2s_log_debug_f32(R2S_FSTR("f"), 0.5f);
    r2s_log_debug_ram("d");
    r2s_log_info_u32(R2S_FSTR("u"), 1u);
    r2s_log_info_i32(R2S_FSTR("i"), -1);
    r2s_log_info_f32(R2S_FSTR("f"), 0.5f);
    r2s_log_info_ram("i");
    r2s_log_warn_i16(R2S_FSTR("i"), -1);
    r2s_log_warn_u32(R2S_FSTR("u"), 1u);
    r2s_log_warn_i32(R2S_FSTR("i"), -1);
    r2s_log_warn_f32(R2S_FSTR("f"), 0.5f);
    r2s_log_warn_ram("w");
    r2s_log_error_u16(R2S_FSTR("u"), 1u);
    r2s_log_error_i16(R2S_FSTR("i"), -1);
    r2s_log_error_u32(R2S_FSTR("u"), 1u);
    r2s_log_error_i32(R2S_FSTR("i"), -1);
    r2s_log_error_f32(R2S_FSTR("f"), 0.5f);
    r2s_log_fatal_u16(R2S_FSTR("u"), 1u);
    r2s_log_fatal_i16(R2S_FSTR("i"), -1);
    r2s_log_fatal_u32(R2S_FSTR("u"), 1u);
    r2s_log_fatal_i32(R2S_FSTR("i"), -1);
    r2s_log_fatal_f32(R2S_FSTR("f"), 0.5f);
    r2s_log_fatal_ram("f");
    TEST_ASSERT_EQUAL_UINT16(24, fake_port_tx_frame_count());
    assert_log(0, 0, 10u, "u", 1u, one_u16, sizeof one_u16);
    assert_log(1, 1, 10u, "i", 2u, neg_i16, sizeof neg_i16);
    assert_log(2, 2, 10u, "f", 5u, half_f32, sizeof half_f32);
    assert_log(3, 3, 10u, "d", 0u, NULL, 0u);
    assert_log(4, 4, 20u, "u", 3u, one_u32, sizeof one_u32);
    assert_log(5, 5, 20u, "i", 4u, neg_i32, sizeof neg_i32);
    assert_log(6, 6, 20u, "f", 5u, half_f32, sizeof half_f32);
    assert_log(7, 7, 20u, "i", 0u, NULL, 0u);
    assert_log(8, 8, 30u, "i", 2u, neg_i16, sizeof neg_i16);
    assert_log(9, 9, 30u, "u", 3u, one_u32, sizeof one_u32);
    assert_log(10, 10, 30u, "i", 4u, neg_i32, sizeof neg_i32);
    assert_log(11, 11, 30u, "f", 5u, half_f32, sizeof half_f32);
    assert_log(12, 12, 30u, "w", 0u, NULL, 0u);
    assert_log(13, 13, 40u, "u", 1u, one_u16, sizeof one_u16);
    assert_log(14, 14, 40u, "i", 2u, neg_i16, sizeof neg_i16);
    assert_log(15, 15, 40u, "u", 3u, one_u32, sizeof one_u32);
    assert_log(16, 16, 40u, "i", 4u, neg_i32, sizeof neg_i32);
    assert_log(17, 17, 40u, "f", 5u, half_f32, sizeof half_f32);
    assert_log(18, 18, 50u, "u", 1u, one_u16, sizeof one_u16);
    assert_log(19, 19, 50u, "i", 2u, neg_i16, sizeof neg_i16);
    assert_log(20, 20, 50u, "u", 3u, one_u32, sizeof one_u32);
    assert_log(21, 21, 50u, "i", 4u, neg_i32, sizeof neg_i32);
    assert_log(22, 22, 50u, "f", 5u, half_f32, sizeof half_f32);
    assert_log(23, 23, 50u, "f", 0u, NULL, 0u);
}

void test_log_null_text_sends_nothing(void)
{
    r2s_log_info(NULL);
    r2s_log_info_u16(NULL, 0u);
    r2s_log_info_ram(NULL);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

void test_log_text_past_the_varint_range_sends_nothing(void)
{
    static char huge[R2S_VARINT_MAX + 2u];

    memset(huge, 'y', sizeof huge - 1u);
    huge[sizeof huge - 1u] = '\0';
    r2s_log_info(R2S_FSTR(huge));
    r2s_log_info_u16(R2S_FSTR(huge), 0u);
    r2s_log_info_ram(huge);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    R2S_LOG_INFO("kept");
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_log(0, 0, 20u, "kept", 0u, NULL, 0u);
}
