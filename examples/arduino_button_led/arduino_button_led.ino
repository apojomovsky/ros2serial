// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// Publish the debounced button state and receive LED commands independently.
// The button reads BUTTON_PIN, active low: 0 on the classic ESP32 and the S3,
// 9 on the C3 and C6, which is their BOOT button, and 2 elsewhere, which is
// the Uno and the Mega. The LED is LED_BUILTIN, or GPIO2 on the boards whose
// variant names none, the classic ESP32 and the DevKit modules. On the S3 and
// the C3 LED_BUILTIN drives the addressable RGB LED through digitalWrite.
#include <ros2serial.h>
#include <std_msgs/msg/bool.hpp>

#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)
#define BUTTON_PIN 0
#elif defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
#define BUTTON_PIN 9
#else
#define BUTTON_PIN 2
#endif

/* The classic ESP32 variant names no built-in LED, so the example falls back
 * to GPIO2, the LED pin on the common DevKit boards and a strapping pin. */
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

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
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_BUILTIN, OUTPUT);
    last_reading = stable_reading = (uint8_t)digitalRead(BUTTON_PIN);
    changed_at = millis();
    node.begin(Serial, 115200);
}

void loop()
{
    node.spin_some();
    const unsigned long now = millis();
    const uint8_t reading = (uint8_t)digitalRead(BUTTON_PIN);
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
