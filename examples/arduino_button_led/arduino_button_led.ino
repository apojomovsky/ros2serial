// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// Publish the debounced button state and receive LED commands independently.
// Pin 2 is active low; LED_BUILTIN is active high on Uno and Mega.
#include <ros2serial.h>
#include <std_msgs/msg/bool.hpp>

r2s::Node node("panel");
r2s::Publisher<std_msgs::msg::Bool> button_pub(node, "button");

void on_led(const std_msgs::msg::Bool &msg)
{
    digitalWrite(LED_BUILTIN, msg.data ? HIGH : LOW);
}
r2s::Subscription<std_msgs::msg::Bool> led_sub(node, "led", on_led);

static uint8_t last_reading;
static uint8_t stable_reading;
static unsigned long changed_at;
static unsigned long published_at;

void setup()
{
    pinMode(2, INPUT_PULLUP);
    pinMode(LED_BUILTIN, OUTPUT);
    last_reading = stable_reading = (uint8_t)digitalRead(2);
    changed_at = millis();
    node.begin(Serial, 115200);
}

void loop()
{
    node.spin_some();
    const unsigned long now = millis();
    const uint8_t reading = (uint8_t)digitalRead(2);
    if (reading != last_reading) {
        changed_at = now;
        last_reading = reading;
    }
    if (now - changed_at >= 20u) {
        stable_reading = reading;
    }
    // Repeat state for late subscribers and reconnects, without blocking spin.
    if (node.connected() && now - published_at >= 100u) {
        std_msgs::msg::Bool msg;
        msg.data = (uint8_t)(stable_reading == LOW);
        if (button_pub.publish(msg) == R2S_OK) {
            published_at = now;
        }
    }
}
