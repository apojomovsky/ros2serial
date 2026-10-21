/* Vector checks for the runner self-test: each record's bytes must match its
 * hex text parsed again in C, which proves the generator transcribes vectors
 * faithfully before any module relies on them. */
#include <stdio.h>

#include "bytes.h"
#include "unity.h"
#include "vectors.h"

static void check_blob(const char *where, const char *covers, const char *field, const uint8_t *bytes,
                       uint16_t len, const char *hex)
{
    static uint8_t parsed[0xFFFF];
    char label[160];
    uint16_t n = r2s_hex_parse(hex, parsed, sizeof parsed);

    snprintf(label, sizeof label, "%s [%s] %s", where, covers, field);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(R2S_HEX_ERROR, n, label);
    r2s_assert_bytes(parsed, n, bytes, len, label);
}

void r2s_vec_check_crc(const r2s_vec_crc_t *v)
{
    check_blob(v->where, v->covers, "data", v->data, v->data_len, v->data_hex);
}

void r2s_vec_check_rcobs(const r2s_vec_rcobs_t *v)
{
    check_blob(v->where, v->covers, "decoded", v->decoded, v->decoded_len, v->decoded_hex);
    check_blob(v->where, v->covers, "encoded", v->encoded, v->encoded_len, v->encoded_hex);
}

void r2s_vec_check_varint(const r2s_vec_varint_t *v)
{
    check_blob(v->where, v->covers, "encoded", v->encoded, v->encoded_len, v->encoded_hex);
}
