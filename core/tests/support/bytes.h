/* Byte strings in the vector notation (spec/protocol.md, section 1), for tests. */
#ifndef R2S_TEST_BYTES_H
#define R2S_TEST_BYTES_H

#include <stddef.h>
#include <stdint.h>

/* Room for the hex text of 64 bytes, enough for a failure message. */
#define R2S_HEX_TEXT_MAX (64u * 3u + 5u)

/* Parses lowercase hex byte pairs separated by single spaces into out.
 * Returns the number of bytes, or -1 if the text is malformed or holds more
 * than max bytes. */
int32_t r2s_hex_parse(const char *hex, uint8_t *out, uint16_t max);

/* Writes the bytes as hex text into out, which must hold at least 8
 * characters; smaller buffers get an empty string. Input that does not fit
 * ends in "..." so a truncated message is never mistaken for the whole. */
void r2s_hex_format(const uint8_t *data, uint16_t len, char *out, size_t size);

/* Fails the current test at the given line, printing both byte strings in hex
 * and the label, unless actual equals expected. Use R2S_ASSERT_BYTES. */
void r2s_assert_bytes_at(const uint8_t *expected, uint16_t expected_len, const uint8_t *actual,
                         uint16_t actual_len, const char *label, unsigned line);

#define R2S_ASSERT_BYTES(expected, expected_len, actual, actual_len, label)                                  \
    r2s_assert_bytes_at((expected), (expected_len), (actual), (actual_len), (label), __LINE__)

#endif
