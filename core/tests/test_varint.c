/* Varint lengths: every length round-trips, the encoding is the shortest one,
 * and an encoding that breaks a rule is refused by that rule. */
#include "r2s/r2s_varint.h"
#include "unity.h"

/* covers: VAR-1, VAR-2 */
void test_varint_every_length_round_trips(void)
{
    for (uint32_t value = 0; value <= R2S_VARINT_MAX; value++) {
        uint8_t encoded[R2S_VARINT_MAX_BYTES];
        uint16_t decoded = 0;
        uint8_t used = 0;
        uint8_t written = r2s_varint_encode((uint16_t)value, encoded);

        TEST_ASSERT_EQUAL_INT8(R2S_VARINT_OK, r2s_varint_decode(encoded, written, &decoded, &used));
        TEST_ASSERT_EQUAL_UINT16((uint16_t)value, decoded);
        TEST_ASSERT_EQUAL_UINT8(written, used);
    }
}

/* covers: VAR-1 */
void test_varint_the_encoding_is_one_two_or_three_bytes_by_magnitude(void)
{
    uint8_t encoded[R2S_VARINT_MAX_BYTES];
    const uint16_t values[] = {0, 127, 128, 16383, 16384, 65535};
    const uint8_t lengths[] = {1, 1, 2, 2, 3, 3};

    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++) {
        uint8_t written = r2s_varint_encode(values[i], encoded);
        TEST_ASSERT_EQUAL_UINT8(lengths[i], written);
    }
}

/* covers: VAR-1 */
void test_varint_the_value_travels_in_the_low_group_first(void)
{
    uint8_t encoded[R2S_VARINT_MAX_BYTES];
    uint8_t written = r2s_varint_encode(300, encoded);

    TEST_ASSERT_EQUAL_UINT8(2, written);
    TEST_ASSERT_EQUAL_HEX8(0xAC, encoded[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02, encoded[1]);
}

/* covers: VAR-4 */
void test_varint_decoding_accepts_only_the_canonical_encoding(void)
{
    const uint8_t encoded[] = {0x80, 0x00};
    uint16_t decoded = 0;
    uint8_t used = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_VARINT_ERR_NOT_SHORTEST,
                           r2s_varint_decode(encoded, sizeof encoded, &decoded, &used));
}

/* covers: VAR-2 */
void test_varint_a_third_byte_with_its_high_bit_set_is_overflow(void)
{
    const uint8_t encoded[] = {0xFF, 0xFF, 0xFF, 0x01};
    uint16_t decoded = 0;
    uint8_t used = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_VARINT_ERR_OVERFLOW,
                           r2s_varint_decode(encoded, sizeof encoded, &decoded, &used));
}

/* covers: VAR-2 */
void test_varint_a_value_over_65535_is_overflow(void)
{
    const uint8_t encoded[] = {0xFF, 0xFF, 0x7F};
    uint16_t decoded = 0;
    uint8_t used = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_VARINT_ERR_OVERFLOW,
                           r2s_varint_decode(encoded, sizeof encoded, &decoded, &used));
}

/* covers: VAR-2 */
void test_varint_a_third_byte_with_its_high_bit_set_is_overflow_at_its_own_length(void)
{
    /* The varint ends with the third byte, so the payload does not cut it
     * short: the rule that refuses it is VAR-2, not VAR-3. */
    const uint8_t encoded[] = {0xFF, 0xFF, 0xFF};
    uint16_t decoded = 0;
    uint8_t used = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_VARINT_ERR_OVERFLOW,
                           r2s_varint_decode(encoded, sizeof encoded, &decoded, &used));
}

/* covers: VAR-3 */
void test_varint_a_payload_that_ends_inside_the_varint_is_truncated(void)
{
    const uint8_t encoded[] = {0x80};
    uint16_t decoded = 0;
    uint8_t used = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_VARINT_ERR_TRUNCATED,
                           r2s_varint_decode(encoded, sizeof encoded, &decoded, &used));
}

/* covers: VAR-3 */
void test_varint_an_empty_payload_is_truncated(void)
{
    uint16_t decoded = 0;
    uint8_t used = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_VARINT_ERR_TRUNCATED, r2s_varint_decode(NULL, 0, &decoded, &used));
}

/* covers: VAR-1 */
void test_varint_decoding_stops_at_the_end_of_the_varint(void)
{
    const uint8_t encoded[] = {0xAC, 0x02, 0xFF};
    uint16_t decoded = 0;
    uint8_t used = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_VARINT_OK, r2s_varint_decode(encoded, sizeof encoded, &decoded, &used));
    TEST_ASSERT_EQUAL_UINT16(300, decoded);
    TEST_ASSERT_EQUAL_UINT8(2, used);
}

/* covers: VAR-1 */
void test_varint_the_bytes_after_the_varint_are_left_alone(void)
{
    uint8_t encoded[] = {0x01, 0xAB};
    uint16_t decoded = 0;
    uint8_t used = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_VARINT_OK, r2s_varint_decode(encoded, sizeof encoded, &decoded, &used));
    TEST_ASSERT_EQUAL_UINT16(1, decoded);
    TEST_ASSERT_EQUAL_UINT8(1, used);
    TEST_ASSERT_EQUAL_HEX8(0xAB, encoded[1]);
}
