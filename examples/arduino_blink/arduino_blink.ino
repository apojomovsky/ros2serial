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

/* Blink on the runtime API: a Bool subscription on led drives the LED.
 * Try it with: ros2 topic pub /led std_msgs/msg/Bool "{data: true}" --once
 * (bridge launch as in the README walkthrough). */

#include <ros2serial.h>
#include <std_msgs/msg/bool.hpp>

r2s::Node node("blink");

/* The Dev Module variant names no built-in LED, so the example falls back
 * to GPIO2, the LED pin on the common DevKit boards. */
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

void on_led(const std_msgs::msg::Bool &msg) { digitalWrite(LED_BUILTIN, msg.data ? HIGH : LOW); }
r2s::Subscription<std_msgs::msg::Bool> led_sub(node, "led", on_led);

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    node.begin(Serial, 115200);
}
void loop() { node.spin_some(); }
