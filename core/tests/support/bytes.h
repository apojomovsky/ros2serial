/* Byte strings in the vector notation (spec/protocol.md, section 1), for tests. */
#ifndef R2S_TEST_BYTES_H
#define R2S_TEST_BYTES_H

#include <stddef.h>
#include <stdint.h>

#define R2S_HEX_ERROR 0xFFFFu
/* Room for the hex text of 64 bytes, enough for a failure message. */
#define R2S_HEX_TEXT_MAX (64u * 3u + 5u)

/* Parses lowercase hex byte pairs separated by single spaces into out.
 * Returns the number of bytes, or R2S_HEX_ERROR if the text is malformed or
 * holds more than max bytes. */
uint16_t r2s_hex_parse(const char *hex, uint8_t *out, uint16_t max);

/* Writes the bytes as hex text into out (always NUL-terminated). Input that
 * does not fit ends in "..." so a truncated message is never mistaken for
 * the whole string. */
void r2s_hex_format(const uint8_t *data, uint16_t len, char *out, size_t size);

/* Fails the current test, printing both byte strings in hex and the label,
 * unless actual equals expected. */
void r2s_assert_bytes(const uint8_t *expected, uint16_t expected_len, const uint8_t *actual,
                      uint16_t actual_len, const char *label);

#endif
