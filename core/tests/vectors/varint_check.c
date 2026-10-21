/* Conformance: every varint vector through the core's varint. */
#include <stdio.h>

#include "bytes.h"
#include "r2s/r2s_varint.h"
#include "unity.h"
#include "vectors.h"

/* The rule a refused encoding breaks, as the vector names it. */
static const char *rule_of(int8_t status)
{
    switch (status) {
    case R2S_VARINT_ERR_OVERFLOW:
        return "VAR-2";
    case R2S_VARINT_ERR_TRUNCATED:
        return "VAR-3";
    case R2S_VARINT_ERR_NOT_SHORTEST:
        return "VAR-4";
    default:
        return NULL;
    }
}

void r2s_vec_check_varint(const r2s_vec_varint_t *v)
{
    char label[256];
    uint8_t encoded[R2S_VARINT_MAX_BYTES];
    uint16_t value = 0;
    uint8_t used = 0;
    int8_t status;

    snprintf(label, sizeof label, "%s [%s]", v->where, v->covers);
    if (v->reject != NULL) {
        status = r2s_varint_decode(v->encoded, v->encoded_len, &value, &used);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(v->reject, rule_of(status), label);
        return;
    }

    status = r2s_varint_decode(v->encoded, v->encoded_len, &value, &used);
    TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_VARINT_OK, status, label);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE((uint16_t)v->value, value, label);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(v->encoded_len, used, label);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(r2s_varint_encode((uint16_t)v->value, encoded), used, label);
    R2S_ASSERT_BYTES(v->encoded, v->encoded_len, encoded, used, label);
}
