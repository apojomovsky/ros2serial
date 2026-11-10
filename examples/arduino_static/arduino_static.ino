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

/* Minimal Arduino example: the static API on Uno and Mega, publishing
 * battery and reacting to cmd_vel. The topics are declared in an R2S_TOPICS
 * list, so the example needs no r2s.yaml and no generated output; the
 * library ships the bundled message types. The sketch body below the list
 * matches the runtime example line for line. */

#define R2S_TOPICS(X) \
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)
#include <ros2serial.h>

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

void on_cmd_vel(const geometry_msgs::msg::Twist &msg) { set_motors(msg.linear.x, msg.angular.z); }

std_msgs::msg::Float32 battery;

void setup() { node.begin(Serial, 115200); }
void loop()  { battery.data = read_battery(); battery_pub.publish(battery); node.spin_some(); }
