// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/* The handwritten test firmware data: descriptor tables and the topic
 * configuration for Float32, Twist and a bounded String. The publish wrappers
 * and dispatch live in handwritten_firmware.c, linked only by the tests that
 * use them, so no other test binary inherits their handler. */
#include <stddef.h>

#include "handwritten.h"
#include "r2s/r2s_params.h"

/* A 16-bit table value as two bytes, read from the struct it describes, so the
 * table stays right whatever the target's alignment. Kinds, modes, counts,
 * bounds and record positions are still written out by hand. */
#define R2S_HAND_U16(value) ((uint8_t)((value) & 0xFFu)), ((uint8_t)(((value) >> 8u) & 0xFFu))

/* Type records ([DSC-2]): size, first field, field count. The topics name
 * their types depth-first ([DSC-11]): Float32, Vector3 and Twist for cmd_vel,
 * then String, then Time, Header, Range, Quaternion, Imu, JointState,
 * Transform, TransformStamped and TFMessage. */
static const uint8_t R2S_FLASH r2s_handwritten_types[] = {
  0x04u,
  0x00u,
  0x00u,
  0x00u,
  0x01u,
  0x18u,
  0x00u,
  0x01u,
  0x00u,
  0x03u,
  0x30u,
  0x00u,
  0x04u,
  0x00u,
  0x02u,
  0x09u,
  0x00u,
  0x06u,
  0x00u,
  0x01u,
  R2S_HAND_U16(sizeof(builtin_interfaces__msg__Time)),
  R2S_HAND_U16(7u),
  0x02u,
  R2S_HAND_U16(sizeof(std_msgs__msg__Header)),
  R2S_HAND_U16(9u),
  0x02u,
  R2S_HAND_U16(sizeof(sensor_msgs__msg__Range)),
  R2S_HAND_U16(11u),
  0x07u,
  R2S_HAND_U16(sizeof(geometry_msgs__msg__Quaternion)),
  R2S_HAND_U16(18u),
  0x04u,
  R2S_HAND_U16(sizeof(sensor_msgs__msg__Imu)),
  R2S_HAND_U16(22u),
  0x07u,
  R2S_HAND_U16(sizeof(sensor_msgs__msg__JointState)),
  R2S_HAND_U16(29u),
  0x05u,
  R2S_HAND_U16(sizeof(geometry_msgs__msg__Transform)),
  R2S_HAND_U16(34u),
  0x02u,
  R2S_HAND_U16(sizeof(geometry_msgs__msg__TransformStamped)),
  R2S_HAND_U16(36u),
  0x03u,
  R2S_HAND_U16(sizeof(tf2_msgs__msg__TFMessage)),
  R2S_HAND_U16(39u),
  0x01u,
};

/* Field entries ([DSC-3]): kind, mode, offset, count, aux. The float data is
 * at 0; x, y and z are doubles at 0, 8 and 16; the two nested messages are
 * at 0 and 24 and name the Vector3 record; the string bound is 8. Time is a
 * second and a nanosecond; the header stamps a Time and bounds its frame_id
 * to 24. Range nests the header, then a radiation byte and five floats. The
 * Imu nests the header, a quaternion, fixed covariance arrays of nine
 * doubles and Vector3 pairs, reusing the Vector3 record. JointState nests
 * the header, four strings of up to 12 characters and three bounded runs of
 * four doubles. Transform nests a Vector3 and a quaternion; TransformStamped
 * nests the header, a child frame id of up to 24 characters and a Transform;
 * TFMessage is a bounded run of four TransformStamped. */
static const uint8_t R2S_FLASH r2s_handwritten_fields[] = {
  0x09u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x0Au,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x0Au,
  0x00u,
  0x08u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x0Au,
  0x00u,
  0x10u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x0Cu,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x01u,
  0x00u,
  0x0Cu,
  0x00u,
  0x18u,
  0x00u,
  0x00u,
  0x00u,
  0x01u,
  0x00u,
  0x0Bu,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x00u,
  0x08u,
  0x00u,
  0x06u,
  0x00u,
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x05u,
  0x00u,
  R2S_HAND_U16(4u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(4u),
  0x0Bu,
  0x00u,
  R2S_HAND_U16(offsetof(std_msgs__msg__Header, frame_id)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(24u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(5u),
  0x01u,
  0x00u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Range, radiation_type)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x09u,
  0x00u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Range, field_of_view)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x09u,
  0x00u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Range, min_range)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x09u,
  0x00u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Range, max_range)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x09u,
  0x00u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Range, range)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x09u,
  0x00u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Range, variance)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x0Au,
  0x00u,
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x0Au,
  0x00u,
  R2S_HAND_U16(8u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x0Au,
  0x00u,
  R2S_HAND_U16(16u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x0Au,
  0x00u,
  R2S_HAND_U16(24u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(5u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Imu, orientation)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(7u),
  0x0Au,
  0x01u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Imu, orientation_covariance)),
  R2S_HAND_U16(9u),
  R2S_HAND_U16(0u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Imu, angular_velocity)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(1u),
  0x0Au,
  0x01u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Imu, angular_velocity_covariance)),
  R2S_HAND_U16(9u),
  R2S_HAND_U16(0u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Imu, linear_acceleration)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(1u),
  0x0Au,
  0x01u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__Imu, linear_acceleration_covariance)),
  R2S_HAND_U16(9u),
  R2S_HAND_U16(0u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(0u),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(5u),
  0x0Bu,
  0x02u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__JointState, name)),
  R2S_HAND_U16(4u),
  R2S_HAND_U16(12u),
  0x0Au,
  0x02u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__JointState, position)),
  R2S_HAND_U16(4u),
  R2S_HAND_U16(0u),
  0x0Au,
  0x02u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__JointState, velocity)),
  R2S_HAND_U16(4u),
  R2S_HAND_U16(0u),
  0x0Au,
  0x02u,
  R2S_HAND_U16(offsetof(sensor_msgs__msg__JointState, effort)),
  R2S_HAND_U16(4u),
  R2S_HAND_U16(0u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(offsetof(geometry_msgs__msg__Transform, translation)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(1u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(offsetof(geometry_msgs__msg__Transform, rotation)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(7u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(offsetof(geometry_msgs__msg__TransformStamped, header)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(5u),
  0x0Bu,
  0x00u,
  R2S_HAND_U16(offsetof(geometry_msgs__msg__TransformStamped, child_frame_id)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(24u),
  0x0Cu,
  0x00u,
  R2S_HAND_U16(offsetof(geometry_msgs__msg__TransformStamped, transform)),
  R2S_HAND_U16(0u),
  R2S_HAND_U16(10u),
  0x0Cu,
  0x02u,
  R2S_HAND_U16(offsetof(tf2_msgs__msg__TFMessage, transforms)),
  R2S_HAND_U16(4u),
  R2S_HAND_U16(11u),
};

const r2s_codec_table_t R2S_HANDWRITTEN_TABLE = {r2s_handwritten_types, 13u, r2s_handwritten_fields,
  40u};

/* The sizes pin the entry counts: a missing or extra byte would shift every
 * later entry, so the build fails here instead of in a codec test. */
typedef char r2s_handwritten_check_types[sizeof(r2s_handwritten_types) == 13u * 5u ? 1 : -1];
typedef char r2s_handwritten_check_fields[sizeof(r2s_handwritten_fields) == 40u * 8u ? 1 : -1];

/* The announced topics: battery publishes Float32, cmd_vel subscribes Twist,
 * log_text publishes the bounded String, and cmd_str subscribes it. The
 * hashes stand in for the RIHS01 bytes G8 will emit. */
static const r2s_topic_t R2S_FLASH r2s_handwritten_topics[] = {
  {R2S_HANDWRITTEN_BATTERY_ID,
    R2S_HANDWRITTEN_NARROW,
    {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u},
    "std_msgs/msg/Float32",
    "battery"},
  {R2S_HANDWRITTEN_CMD_VEL_ID,
    (uint8_t)(0x01u | R2S_HANDWRITTEN_NARROW),
    {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u},
    "geometry_msgs/msg/Twist",
    "cmd_vel"},
  {R2S_HANDWRITTEN_LOG_TEXT_ID,
    R2S_HANDWRITTEN_NARROW,
    {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u},
    "std_msgs/msg/String",
    "log_text"},
  {R2S_HANDWRITTEN_CMD_STR_ID,
    (uint8_t)(0x01u | R2S_HANDWRITTEN_NARROW),
    {0x30u, 0x31u, 0x32u, 0x33u, 0x34u, 0x35u, 0x36u, 0x37u},
    "std_msgs/msg/String",
    "cmd_str"},
};

/* The announced parameters for the model-based tests: a bool led defaulting
 * to true, a read-only i64 limit and a bounded string name. The table lives
 * in RAM with the live values, as the session requires. */
#ifdef R2S_WITH_PARAMS
static char r2s_handwritten_name_value[8];
static r2s_param_t r2s_handwritten_params[] = {
  {"led", R2S_PARAM_BOOL, 0u, 0u, {.b = 1u}, {.b = 0u}},
  {"limit", R2S_PARAM_I64, R2S_PARAM_READONLY, 0u, {.i = 7}, {.i = 0}},
  {"name", R2S_PARAM_STRING, 0u, sizeof r2s_handwritten_name_value - 1u, {.s = "ab"}, {.s = NULL}},
};
#endif

r2s_status_t r2s_handwritten_configure(void)
{
  r2s_status_t rc = r2s_session_configure("node", r2s_handwritten_topics, 4u);

  if (rc != R2S_OK) {
    return rc;
  }
#ifdef R2S_WITH_PARAMS
  r2s_handwritten_name_value[0] = '\0';
  r2s_handwritten_params[2].value.s = r2s_handwritten_name_value;
  return r2s_params_configure(r2s_handwritten_params, 3u);
#else
  return R2S_OK;
#endif
}
