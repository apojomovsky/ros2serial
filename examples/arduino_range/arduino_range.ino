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
