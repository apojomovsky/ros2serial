/* The test helpers every core test relies on: hex parsing and byte comparison. */
#include "bytes.h"
#include "unity.h"

void test_hex_parse_valid_string_gives_its_bytes(void)
{
    uint8_t out[3];
    const uint8_t expected[] = {0x10, 0xab, 0x00};

    uint16_t n = r2s_hex_parse("10 ab 00", out, sizeof out);

    TEST_ASSERT_EQUAL_UINT16(3, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 3);
}

void test_hex_parse_empty_string_gives_no_bytes(void)
{
    uint8_t out[1];

    TEST_ASSERT_EQUAL_UINT16(0, r2s_hex_parse("", out, sizeof out));
}

void test_hex_parse_malformed_string_is_an_error(void)
{
    uint8_t out[4];
    const char *bad[] = {"1", "1g", "10  20", "10 ", " 10", "AB", "10-20"};

    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(R2S_HEX_ERROR, r2s_hex_parse(bad[i], out, sizeof out), bad[i]);
    }
}

void test_hex_parse_too_long_for_the_buffer_is_an_error(void)
{
    uint8_t out[2];

    TEST_ASSERT_EQUAL_UINT16(R2S_HEX_ERROR, r2s_hex_parse("01 02 03", out, sizeof out));
}

void test_hex_format_writes_the_vector_notation(void)
{
    const uint8_t in[] = {0x01, 0xfe};
    char text[R2S_HEX_TEXT_MAX];

    r2s_hex_format(in, sizeof in, text, sizeof text);

    TEST_ASSERT_EQUAL_STRING("01 fe", text);
}

void test_hex_format_truncates_long_input_visibly(void)
{
    uint8_t in[200] = {0};
    char text[16];

    r2s_hex_format(in, sizeof in, text, sizeof text);

    TEST_ASSERT_EQUAL_STRING("00 00 00 00 ...", text);
}
