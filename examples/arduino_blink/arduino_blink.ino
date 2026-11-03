/* Blink on the runtime API: a Bool subscription on led drives the LED.
 * Try it with: ros2 topic pub /led std_msgs/msg/Bool "{data: true}" --once
 * (bridge launch as in the README walkthrough). */

#include <ros2serial.h>
#include <std_msgs/msg/bool.hpp>

r2s::Node node("blink");

void on_led(const std_msgs::msg::Bool &msg) { digitalWrite(LED_BUILTIN, msg.data ? HIGH : LOW); }
r2s::Subscription<std_msgs::msg::Bool> led_sub(node, "led", on_led);

void setup()
{
    pinMode(LED_BUILTIN, OUTPUT);
    node.begin(Serial, 115200);
}
void loop() { node.spin_some(); }
