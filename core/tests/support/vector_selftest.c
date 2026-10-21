/* Vector checks for the runner self-test: each record's bytes must match the
 * vector's own text, parsed again in C. The generator writes both from the
 * YAML independently, so a transcription bug on either side shows here. */
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "unity.h"
#include "vectors.h"

static void check_blob(const char *where, const char *covers, const char *field, const uint8_t *bytes,
                       uint16_t len, const char *hex)
{
    static uint8_t parsed[0xFFFF];
    char label[512];
    int32_t n = r2s_hex_parse(hex, parsed, (uint16_t)sizeof parsed);

    snprintf(label, sizeof label, "%s [%s] %s", where, covers, field);
    TEST_ASSERT_TRUE_MESSAGE(n >= 0, label);
    R2S_ASSERT_BYTES(parsed, (uint16_t)n, bytes, len, label);
}

void r2s_vec_check_crc(const r2s_vec_crc_t *v)
{
    if (v->text != NULL) {
        R2S_ASSERT_BYTES((const uint8_t *)v->text, (uint16_t)strlen(v->text), v->data, v->data_len, v->where);
    } else {
        check_blob(v->where, v->covers, "data", v->data, v->data_len, v->data_hex);
    }
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
