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

/* Button on the runtime API: a debounced Bool publisher on button.
 * Pin 2 is active low with its pull-up, true means pressed.
 * Try it with: ros2 topic echo /button std_msgs/msg/Bool
 * (bridge launch as in the README walkthrough). */

#include <ros2serial.h>
#include <std_msgs/msg/bool.hpp>

r2s::Node node("button");
r2s::Publisher<std_msgs::msg::Bool> button_pub(node, "button");

static uint8_t s_stable;
static uint8_t s_last;
static unsigned long s_since;

void setup()
{
    pinMode(2, INPUT_PULLUP);
    s_stable = s_last = (uint8_t)digitalRead(2);
    s_since = millis();
    node.begin(Serial, 115200);
}
void loop()
{
    uint8_t reading = (uint8_t)digitalRead(2);
    std_msgs::msg::Bool msg;

    if (reading != s_last) {
        s_since = millis();
    }
    s_last = reading;
    if (reading != s_stable && (millis() - s_since) > 20u) {
        s_stable = reading;
        msg.data = (uint8_t)(reading == LOW ? 1u : 0u);
        button_pub.publish(msg);
    }
    node.spin_some();
}
