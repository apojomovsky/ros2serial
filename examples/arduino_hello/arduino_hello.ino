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

/* Hello world on the runtime API: a String publisher on chatter.
 * Try it with: ros2 topic echo /chatter std_msgs/msg/String --once
 * (bridge launch as in the README walkthrough). */

#include <stdio.h>

#include <ros2serial.h>
#include <std_msgs/msg/string.hpp>

r2s::Node node("hello");
r2s::Publisher<std_msgs::msg::String> chatter_pub(node, "chatter");

static unsigned long s_count;
static unsigned long s_last;

void setup() { node.begin(Serial, 115200); }
void loop()
{
    // Twice a second is plenty for a greeting; a publish every loop
    // would flood the link and hit TX_BUSY instead.
    if (millis() - s_last >= 500u) {
        std_msgs::msg::String msg;

        s_last = millis();
        snprintf(msg.data, sizeof(msg.data), "hello %lu", s_count++);
        chatter_pub.publish(msg);
    }
    node.spin_some();
}
