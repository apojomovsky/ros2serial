/* Slice firmware for the avr end-to-end suite: the four topics the posix
 * virtual MCU announces, on an emulated Uno or Mega under simavr. The
 * sketch declares its topics in an R2S_TOPICS list, so no r2s.yaml and no
 * generator step are involved; battery publishes at the 50 Hz scenario 2
 * measures, and every cmd_vel frame echoes back for scenario 3. */

#define R2S_NODE_NAME "r2s_slice"
#define R2S_TOPICS(X) \
    X(PUB, battery, std_msgs, Float32) \
    X(SUB, cmd_vel, geometry_msgs, Twist) \
    X(PUB, echo_vel, geometry_msgs, Twist) \
    X(PUB, log_text, std_msgs, String)

#include <ros2serial.h>

static uint32_t s_sent;
static uint8_t s_log_booted;

/* Best effort echo for scenario 3: five messages on an idle link never fill
 * the port, so the return carries no signal the test would use. */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)r2s_publish_echo_vel(msg);
}

/* One log_text line: the value as 8 hex digits. Fixed width fits the
 * bundled String bound of 16 characters for every id. */
static void publish_log_text(uint32_t value)
{
    std_msgs__msg__String msg;
    uint8_t i;

    for (i = 0u; i < 8u; i++) {
        uint8_t digit = (uint8_t)((value >> 28u) & 0xFu);

        msg.data[i] = (char)(digit < 10u ? (char)('0' + digit) : (char)('a' + digit - 10u));
        value <<= 4u;
    }
    msg.data[8] = '\0';
    (void)r2s_publish_log_text(&msg);
}

void setup(void)
{
    Serial.begin(115200);
    if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return;
    }
    r2s_init();
}

void loop(void)
{
    static uint32_t last_pub;
    uint32_t now;

    r2s_spin_some();
    now = r2s_port_tick();
    if (r2s_connected() == 0u) {
        return;
    }
    if (s_log_booted == 0u) {
        /* A boot line that fails to publish is retried until it sends. It
         * carries the assigned session token, so the host sees what the
         * firmware stored. */
        publish_log_text(r2s_session_token());
        s_log_booted = 1u;
    }
    if ((uint32_t)(now - last_pub) >= 20u) {
        std_msgs__msg__Float32 msg;

        msg.data = (float)s_sent;
        if (r2s_publish_battery(&msg) == R2S_OK) {
            s_sent++;
            last_pub = now;
            if (s_sent % 10u == 0u) {
                publish_log_text(s_sent);
            }
        }
    }
}
