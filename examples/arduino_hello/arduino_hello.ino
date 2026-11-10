// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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
