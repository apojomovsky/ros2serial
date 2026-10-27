/* Conformance: every message vector through the codec interpreter, encoding
 * the vector's message and decoding its payload. */
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "handwritten.h"
#include "r2s/r2s_codec.h"
#include "unity.h"
#include "vectors.h"

void r2s_vec_check_messages(const r2s_vec_messages_t *v)
{
    char label[256];
    uint8_t storage[sizeof(geometry_msgs__msg__Twist)];
    uint8_t out[R2S_MAX_FRAME];
    r2s_codec_out_t sink = {out, (uint16_t)sizeof out, 0u, 0u};
    uint8_t field = 0xFFu;
    uint16_t type;
    uint16_t size;

    snprintf(label, sizeof label, "%s [%s]", v->where, v->covers);
    if (strcmp(v->type, "std_msgs/msg/Float32") == 0) {
        type = R2S_HANDWRITTEN_FLOAT32;
        size = (uint16_t)sizeof(std_msgs__msg__Float32);
    } else if (strcmp(v->type, "geometry_msgs/msg/Twist") == 0) {
        type = R2S_HANDWRITTEN_TWIST;
        size = (uint16_t)sizeof(geometry_msgs__msg__Twist);
    } else {
        TEST_FAIL_MESSAGE("the C runner knows Float32 and Twist");
        return;
    }
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(size, v->encode_len, label);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(size, v->decode_len, label);
    memcpy(storage, v->encode, size);
    TEST_ASSERT_EQUAL_INT8_MESSAGE(
        R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, type, storage, v->narrow, &sink, &field), label);
    R2S_ASSERT_BYTES(v->payload, v->payload_len, out, sink.len, label);
    memset(storage, 0xA5u, size);
    TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK,
                                   r2s_codec_decode(&R2S_HANDWRITTEN_TABLE, type, v->payload, v->payload_len,
                                                    v->narrow, storage, &field),
                                   label);
    R2S_ASSERT_BYTES(v->decode, v->decode_len, storage, size, label);
}
