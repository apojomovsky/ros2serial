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
    /* Sized by the largest handwritten struct, so every vector image fits;
     * the assert below turns a future larger type into a clean failure. */
  uint8_t storage[sizeof(tf2_msgs__msg__TFMessage)];
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
  } else if (strcmp(v->type, "sensor_msgs/msg/Range") == 0) {
    type = R2S_HANDWRITTEN_RANGE;
    size = (uint16_t)sizeof(sensor_msgs__msg__Range);
  } else if (strcmp(v->type, "sensor_msgs/msg/Imu") == 0) {
    type = R2S_HANDWRITTEN_IMU;
    size = (uint16_t)sizeof(sensor_msgs__msg__Imu);
  } else if (strcmp(v->type, "sensor_msgs/msg/JointState") == 0) {
    type = R2S_HANDWRITTEN_JOINT_STATE;
    size = (uint16_t)sizeof(sensor_msgs__msg__JointState);
  } else if (strcmp(v->type, "geometry_msgs/msg/TransformStamped") == 0) {
    type = R2S_HANDWRITTEN_TRANSFORM_STAMPED;
    size = (uint16_t)sizeof(geometry_msgs__msg__TransformStamped);
  } else if (strcmp(v->type, "tf2_msgs/msg/TFMessage") == 0) {
    type = R2S_HANDWRITTEN_TF_MESSAGE;
    size = (uint16_t)sizeof(tf2_msgs__msg__TFMessage);
  } else {
    TEST_FAIL_MESSAGE("the C runner knows Float32, Twist, Range, Imu, JointState, "
                          "TransformStamped and TFMessage");
    return;
  }
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(size, v->encode_len, label);
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(size, v->decode_len, label);
  TEST_ASSERT_TRUE_MESSAGE(size <= (uint16_t)sizeof storage, label);
  memcpy(storage, v->encode, size);
  TEST_ASSERT_EQUAL_INT8_MESSAGE(
        R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, type, storage, v->narrow, &sink, &field),
    label);
  R2S_ASSERT_BYTES(v->payload, v->payload_len, out, sink.len, label);
  memset(storage, 0xA5u, size);
  TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK,
                                   r2s_codec_decode(&R2S_HANDWRITTEN_TABLE, type, v->payload,
    v->payload_len,
                                                    v->narrow, storage, &field),
                                   label);
  R2S_ASSERT_BYTES(v->decode, v->decode_len, storage, size, label);
}
