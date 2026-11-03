/* Range on the runtime API: a sonar-style Range publisher on range.
 * The reading stands in for the hardware, fixed at one metre.
 * Try it with: ros2 topic echo /range sensor_msgs/msg/Range --once
 * (bridge launch as in the README walkthrough). */

#include <ros2serial.h>
#include <sensor_msgs/msg/range.hpp>

r2s::Node node("range");
r2s::Publisher<sensor_msgs::msg::Range> range_pub(node, "range");

static float read_sonar(void) { return 1.0f; }

void setup() { node.begin(Serial, 115200); }
void loop()
{
    sensor_msgs::msg::Range msg;

    msg.header.stamp.sec = 0;
    msg.header.stamp.nanosec = 0u;
    msg.header.frame_id[0] = '\0';
    msg.radiation_type = 0u;
    msg.field_of_view = 0.26f;
    msg.min_range = 0.02f;
    msg.max_range = 4.0f;
    msg.range = read_sonar();
    msg.variance = 0.0f;
    range_pub.publish(msg);
    node.spin_some();
}
