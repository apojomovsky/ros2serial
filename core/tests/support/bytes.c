/* Hex parsing, formatting and byte comparison for tests. */
#include "bytes.h"

#include <stdio.h>
#include <string.h>

#include "unity.h"

static int nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

int32_t r2s_hex_parse(const char *hex, uint8_t *out, uint16_t max)
{
    uint16_t n = 0;
    const char *p = hex;

    while (*p != '\0') {
        int hi = nibble(p[0]);
        int lo = hi < 0 ? -1 : nibble(p[1]);
        if (lo < 0 || n == max) {
            return -1;
        }
        out[n++] = (uint8_t)(hi * 16 + lo);
        p += 2;
        if (*p == ' ') {
            p++;
            if (*p == '\0') {
                return -1;
            }
        } else if (*p != '\0') {
            return -1;
        }
    }
    return n;
}

void r2s_hex_format(const uint8_t *data, uint16_t len, char *out, size_t size)
{
    static const char digits[] = "0123456789abcdef";
    size_t pos = 0;

    if (size < 8u) {
        if (size > 0u) {
            out[0] = '\0';
        }
        return;
    }
    out[0] = '\0';
    for (uint16_t i = 0; i < len; i++) {
        /* Each byte needs 3 characters, and " ..." plus the NUL must still fit. */
        if (pos + 3u + 5u > size) {
            memcpy(out + pos, pos > 0 ? " ..." : "...", pos > 0 ? 5u : 4u);
            return;
        }
        if (i > 0) {
            out[pos++] = ' ';
        }
        out[pos++] = digits[data[i] >> 4];
        out[pos++] = digits[data[i] & 0x0Fu];
        out[pos] = '\0';
    }
}

void r2s_assert_bytes_at(const uint8_t *expected, uint16_t expected_len, const uint8_t *actual,
                         uint16_t actual_len, const char *label, unsigned line)
{
    char want[R2S_HEX_TEXT_MAX];
    char got[R2S_HEX_TEXT_MAX];
    char message[2u * R2S_HEX_TEXT_MAX + 512u];

    if (expected_len == actual_len && (expected_len == 0 || memcmp(expected, actual, expected_len) == 0)) {
        return;
    }
    r2s_hex_format(expected, expected_len, want, sizeof want);
    r2s_hex_format(actual, actual_len, got, sizeof got);
    snprintf(message, sizeof message, "%s: expected [%s] (%u bytes), got [%s] (%u bytes)", label, want,
             (unsigned)expected_len, got, (unsigned)actual_len);
    UNITY_TEST_FAIL(line, message);
}
