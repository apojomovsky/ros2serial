/* The handwritten test firmware data: descriptor tables and the topic
 * configuration for Float32, Twist and a bounded String. The publish wrappers
 * and dispatch live in handwritten_firmware.c, linked only by the tests that
 * use them, so no other test binary inherits their handler. */
#include "handwritten.h"

/* Type records ([DSC-2]): size, first field, field count. Float32 is 4 bytes
 * with 1 field; Vector3 is 24 bytes with 3; Twist is 48 bytes with 2, and its
 * fields start after Vector3's 3 entries ([DSC-9]); String is 9 bytes with 1.
 * The topics name their types depth-first ([DSC-11]): Float32, Vector3 and
 * Twist for cmd_vel, then String. */
static const uint8_t R2S_FLASH r2s_handwritten_types[] = {
    0x04u, 0x00u, 0x00u, 0x00u, 0x01u, 0x18u, 0x00u, 0x01u, 0x00u, 0x03u,
    0x30u, 0x00u, 0x04u, 0x00u, 0x02u, 0x09u, 0x00u, 0x06u, 0x00u, 0x01u,
};

/* Field entries ([DSC-3]): kind, mode, offset, count, aux. The float data is
 * at 0; x, y and z are doubles at 0, 8 and 16; the two nested messages are
 * at 0 and 24 and name the Vector3 record; the string bound is 8. */
static const uint8_t R2S_FLASH r2s_handwritten_fields[] = {
    0x09u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x0Au, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    0x00u, 0x00u, 0x0Au, 0x00u, 0x08u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x0Au, 0x00u, 0x10u, 0x00u,
    0x00u, 0x00u, 0x00u, 0x00u, 0x0Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x0Cu, 0x00u,
    0x18u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x0Bu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x08u, 0x00u,
};

const r2s_codec_table_t R2S_HANDWRITTEN_TABLE = {r2s_handwritten_types, 4u, r2s_handwritten_fields, 7u};

/* The announced topics: battery publishes Float32, cmd_vel subscribes Twist
 * and log_text publishes the bounded String. The hashes stand in for the
 * RIHS01 bytes G8 will emit. */
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
};

r2s_status_t r2s_handwritten_configure(void)
{
    return r2s_session_configure("node", r2s_handwritten_topics, 3u);
}
