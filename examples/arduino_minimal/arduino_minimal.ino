/* Minimal Arduino example: the runtime API on Uno and Mega, publishing
 * battery and reacting to cmd_vel. The topics are registered while the
 * firmware runs, so the example needs no r2s.yaml and no generated output;
 * the library ships the runtime layer and the bundled message types. */

#include <ros2serial.h>
#include <std_msgs/msg/float32.hpp>
#include <geometry_msgs/msg/twist.hpp>

/* The board hooks the sketch calls. A real firmware drives the motors and
 * reads the battery from its own hardware, so the example keeps them
 * empty and stands in for that hardware. */
static void set_motors(double linear, double angular)
{
    (void)linear;
    (void)angular;
}

static float read_battery(void)
{
    return 0.0f;
}

r2s::Node node("base_controller");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");

void on_cmd_vel(const geometry_msgs::msg::Twist &msg) { set_motors(msg.linear.x, msg.angular.z); }
r2s::Subscription<geometry_msgs::msg::Twist> cmd_vel_sub(node, "cmd_vel", on_cmd_vel);

std_msgs::msg::Float32 battery;

void setup() { node.begin(Serial, 115200); }

/* An example publishes a fresh reading, so a missed interval is dropped,
 * not replayed: the deadline moves one period per publish and resyncs to
 * now when more than one period late, and the offline branch resets it
 * the same way. A stall then skips its frames instead of bursting them;
 * the posix test firmware owes every tick, which its count needs. */
void loop()
{
    static uint32_t last_pub;
    uint32_t now;

    node.spin_some();
    now = node.now();
    if (!node.connected()) {
        last_pub = now;
        return;
    }
    if ((uint32_t)(now - last_pub) >= 100u) {
        battery.data = read_battery();
        if (battery_pub.publish(battery) == R2S_OK) {
            last_pub += 100u;
            if ((uint32_t)(now - last_pub) >= 100u) {
                last_pub = now;
            }
        }
    }
}
