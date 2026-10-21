/* The test helpers every core test relies on: hex parsing and byte comparison. */
#include "bytes.h"
#include "unity.h"

void test_hex_parse_valid_string_gives_its_bytes(void)
{
    uint8_t out[3];
    const uint8_t expected[] = {0x10, 0xab, 0x00};

    int32_t n = r2s_hex_parse("10 ab 00", out, sizeof out);

    TEST_ASSERT_EQUAL_INT32(3, n);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, out, 3);
}

void test_hex_parse_empty_string_gives_no_bytes(void)
{
    uint8_t out[1];

    TEST_ASSERT_EQUAL_INT32(0, r2s_hex_parse("", out, sizeof out));
}

void test_hex_parse_malformed_string_is_an_error(void)
{
    uint8_t out[4];
    const char *bad[] = {"1", "1g", "10  20", "10 ", " 10", "AB", "10-20"};

    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_EQUAL_INT32_MESSAGE(-1, r2s_hex_parse(bad[i], out, sizeof out), bad[i]);
    }
}

void test_hex_parse_too_long_for_the_buffer_is_an_error(void)
{
    uint8_t out[2];

    TEST_ASSERT_EQUAL_INT32(-1, r2s_hex_parse("01 02 03", out, sizeof out));
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

void test_hex_format_tiny_buffer_gets_an_empty_string(void)
{
    const uint8_t in[] = {0x01};
    char text[4] = "xyz";

    r2s_hex_format(in, sizeof in, text, sizeof text);

    TEST_ASSERT_EQUAL_STRING("", text);
}

void test_assert_bytes_equal_strings_pass(void)
{
    const uint8_t a[] = {0x01, 0x02};
    const uint8_t b[] = {0x01, 0x02};

    R2S_ASSERT_BYTES(a, 2, b, 2, "equal");
    R2S_ASSERT_BYTES(NULL, 0, NULL, 0, "both empty");
}
