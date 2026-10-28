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

/* A reject rule is either absent or a real ID; an empty string means a lost transcription. */
static void check_rule(const char *where, const char *rule)
{
    TEST_ASSERT_TRUE_MESSAGE(rule == NULL || strlen(rule) >= 5u, where);
}

void r2s_vec_check_rcobs(const r2s_vec_rcobs_t *v)
{
    check_rule(v->where, v->reject);
    check_blob(v->where, v->covers, "decoded", v->decoded, v->decoded_len, v->decoded_hex);
    check_blob(v->where, v->covers, "encoded", v->encoded, v->encoded_len, v->encoded_hex);
}

void r2s_vec_check_varint(const r2s_vec_varint_t *v)
{
    check_rule(v->where, v->reject);
    check_blob(v->where, v->covers, "encoded", v->encoded, v->encoded_len, v->encoded_hex);
}

void r2s_vec_check_frames(const r2s_vec_frames_t *v)
{
    check_blob(v->where, v->covers, "payload", v->payload, v->payload_len, v->payload_hex);
    check_blob(v->where, v->covers, "frame", v->frame, v->frame_len, v->frame_hex);
    check_blob(v->where, v->covers, "wire", v->wire, v->wire_len, v->wire_hex);
}

void r2s_vec_check_messages(const r2s_vec_messages_t *v)
{
    check_blob(v->where, v->covers, "payload", v->payload, v->payload_len, v->payload_hex);
}

void r2s_vec_check_session(const r2s_vec_session_t *v)
{
    char label[512];

    snprintf(label, sizeof label, "%s [%s] %s", v->where, v->covers, v->name);
    TEST_ASSERT_TRUE_MESSAGE(v->nsteps > 0u, label);
    for (uint16_t i = 0u; i < v->nsteps; i++) {
        snprintf(label, sizeof label, "%s [%s] %s step %u", v->where, v->covers, v->name, i);
        TEST_ASSERT_TRUE_MESSAGE(v->steps[i].from_host <= 1u, label);
        check_blob(v->where, v->covers, "frame", v->steps[i].frame, v->steps[i].frame_len,
                   v->steps[i].frame_hex);
    }
}

void r2s_vec_check_time(const r2s_vec_time_t *v)
{
    char label[512];
    uint32_t khz = v->tick_hz / 1000u;

    snprintf(label, sizeof label, "%s [%s]", v->where, v->covers);
    TEST_ASSERT_TRUE_MESSAGE(v->tick_hz >= 1000u && v->tick_hz % 1000u == 0u, label);
    TEST_ASSERT_TRUE_MESSAGE(khz != 0u && 1000000u % khz == 0u, label);
    if (v->synced != 0u) {
        TEST_ASSERT_TRUE_MESSAGE(v->nanosec < 1000000000u, label);
    } else {
        TEST_ASSERT_EQUAL_INT32_MESSAGE(0, v->sec, label);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, v->nanosec, label);
    }
}
