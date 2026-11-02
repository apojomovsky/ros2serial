/* Minimal X-macro example: the bundled library on Uno and Mega, publishing
 * battery and greeting and counting cmd_vel and joints frames. The sketch
 * declares its topics in an R2S_TOPICS list, so there is no r2s.yaml and no
 * generator step; the bundled descriptors compile in the library, which
 * never sees the list. The header carries C linkage, so callback definitions
 * keep it with no extern "C" block, and the port glue over Serial lives in
 * the library. */

#define R2S_NODE_NAME "hello_xmacro"
#define R2S_TOPICS(X) \
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
    X(Publisher, std_msgs::msg::String, greeting_pub, "greeting") \
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel) \
    X(Subscription, sensor_msgs::msg::JointState, joints_sub, "joints", on_joints)

/* The core needs the bundled limits as defines (R2S_WITH_CODEC and the
 * R2S_MAX_* values from r2s_bundled.h); the example build passes them
 * through compiler.c.extra_flags, since a library sets none of its own. */
#include <ros2serial.h>

static uint32_t s_cmd_vel_count;
static uint32_t s_joints_count;
static uint32_t s_sent;

/* Handle cmd_vel and joints; dispatch calls them from r2s_spin_some(). */
void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    (void)msg;
    s_cmd_vel_count++;
}

void on_joints(const sensor_msgs::msg::JointState &msg)
{
    (void)msg;
    s_joints_count++;
}

void setup(void)
{
    Serial.begin(115200);
    if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return;
    }
    r2s_init();
}

/* An example publishes a fresh reading, so a missed interval is dropped,
 * not replayed: the deadline moves one period per publish and resyncs to
 * now when more than one period late, and the offline branch resets it
 * the same way. A stall then skips its frames instead of bursting them;
 * the posix test firmware owes every tick, which its count needs. */
void loop(void)
{
    static uint32_t last_pub;
    uint32_t now;

    r2s_spin_some();
    now = r2s_port_tick();
    if (r2s_connected() == 0u) {
        last_pub = now;
        return;
    }
    if ((uint32_t)(now - last_pub) >= 100u) {
        std_msgs::msg::Float32 battery = {(float)s_sent};
        std_msgs::msg::String greeting = {};

        if (r2s_publish_battery_pub(&battery) == R2S_OK && r2s_publish_greeting_pub(&greeting) == R2S_OK) {
            s_sent++;
            last_pub += 100u;
            if ((uint32_t)(now - last_pub) >= 100u) {
                last_pub = now;
            }
        }
    }
}
