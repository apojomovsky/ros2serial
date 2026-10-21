/* Conformance: every rCOBS vector through the core's rCOBS, in both
 * directions. */
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "r2s/r2s_rcobs.h"
#include "unity.h"
#include "vectors.h"

/* The rule a refused encoding breaks, as the vector names it. */
static const char *rule_of(int8_t status)
{
    switch (status) {
    case R2S_RCOBS_ERR_CODE:
        return "COB-5";
    case R2S_RCOBS_ERR_TRAILING:
        return "COB-6";
    default:
        return NULL;
    }
}

/* Encodes data into out and returns the length of the encoding. */
static uint16_t encode(const uint8_t *data, uint16_t len, uint8_t *out)
{
    r2s_rcobs_encoder_t enc;
    uint16_t written = 0;

    r2s_rcobs_encoder_init(&enc);
    for (uint16_t i = 0; i < len; i++) {
        written = (uint16_t)(written + r2s_rcobs_encode_byte(&enc, data[i], out + written));
    }
    return (uint16_t)(written + r2s_rcobs_encode_end(&enc, out + written));
}

void r2s_vec_check_rcobs(const r2s_vec_rcobs_t *v)
{
    static uint8_t buffer[0xFFFF];
    char label[256];
    uint8_t *decoded = NULL;
    int8_t status;

    snprintf(label, sizeof label, "%s [%s]", v->where, v->covers);
    /* The vectors hold the delimiter with the encoding; the codec does not
     * know it, so the check takes it off. */
    uint16_t len = (uint16_t)(v->encoded_len - 1u);
    memcpy(buffer, v->encoded, len);

    if (v->reject != NULL) {
        status = r2s_rcobs_decode(buffer, len, &decoded);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(v->reject, rule_of(status), label);
        return;
    }

    status = r2s_rcobs_decode(buffer, len, &decoded);
    TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_RCOBS_OK, status, label);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(v->decoded_len, (uint16_t)(len - (uint16_t)(decoded - buffer)), label);
    R2S_ASSERT_BYTES(v->decoded, v->decoded_len, decoded, v->decoded_len, label);

    /* The other direction: the encoding of the decoded frame, with the
     * delimiter the framer adds, is the encoding the vector holds. */
    uint8_t encoded[0xFFFF];
    uint16_t encoded_len = encode(v->decoded, v->decoded_len, encoded);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(len, encoded_len, label);
    R2S_ASSERT_BYTES(v->encoded, (uint16_t)(v->encoded_len - 1u), encoded, encoded_len, label);
}
