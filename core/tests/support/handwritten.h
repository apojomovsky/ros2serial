/* Handwritten message code for the codec tests: C structs, descriptor tables
 * and the topic configuration for Float32 and Twist. It stands in for the
 * files G7 and G8 will generate, with the same bytes (spec/protocol.md,
 * section 9), until G8 replaces it in its own test firmware. */
#ifndef R2S_TEST_HANDWRITTEN_H
#define R2S_TEST_HANDWRITTEN_H

#include <float.h>
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_session.h"

/* The C structs the descriptors below describe (docs/00-design.md, "C type
 * mapping"): one float, and two nested triples of doubles. */
typedef struct {
    float data;
} std_msgs__msg__Float32;

typedef struct {
    double x;
    double y;
    double z;
} geometry_msgs__msg__Vector3;

typedef struct {
    geometry_msgs__msg__Vector3 linear;
    geometry_msgs__msg__Vector3 angular;
} geometry_msgs__msg__Twist;

/* Type indices in the handwritten table, in the depth-first order [DSC-11]
 * fixes: each nested type is numbered before the type that names it. */
#define R2S_HANDWRITTEN_FLOAT32 0u
#define R2S_HANDWRITTEN_VECTOR3 1u
#define R2S_HANDWRITTEN_TWIST 2u

/* The user topic IDs of the test firmware. */
#define R2S_HANDWRITTEN_BATTERY_ID 16u
#define R2S_HANDWRITTEN_CMD_VEL_ID 17u

/* The narrow flag every topic carries on a build whose double is 32 bits,
 * where the flag is the truth for the wire ([ENC-12], [CTL-5]). */
#if DBL_MANT_DIG == FLT_MANT_DIG
#define R2S_HANDWRITTEN_NARROW 0x02u
#else
#define R2S_HANDWRITTEN_NARROW 0x00u
#endif

/* The handwritten descriptor tables, as [DSC-2] and [DSC-3] lay them out. */
extern const r2s_codec_table_t R2S_HANDWRITTEN_TABLE;

/* Configures the session with the test firmware's node name and topics. Call
 * before r2s_init(); it returns what r2s_session_configure() returns. */
r2s_status_t r2s_handwritten_configure(void);

/* Publishes on battery; the generated file will own this shape. Fails with
 * the not-connected error while announcing ([SES-4]), with the rejected
 * error when the host did not accept the topic, and with the bounds error
 * when the message does not fit, sending nothing in each case. */
r2s_status_t r2s_publish_battery(const std_msgs__msg__Float32 *msg);

/* Handles a cmd_vel frame. The test binary implements it; dispatch calls it
 * from r2s_spin_some(), never from inside a publish. */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg);

#endif
