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

/* Servo on the runtime API: a Float32 subscription on servo sets the angle.
 * A 50 Hz pulse drives the hobby servo, 1 ms at 0 degrees to 2 ms at 180,
 * so the link never blocks for the pulse. Timer1 makes the pulse on AVR
 * (OC1A is pin 9 on the Uno and pin 11 on the Mega, and taking Timer1 stops
 * analogWrite on its two pins); LEDC makes it on ESP32.
 * Try it with: ros2 topic pub /servo std_msgs/msg/Float32 "{data: 90.0}" --once
 * (bridge launch as in the README walkthrough). */

#include <ros2serial.h>
#include <std_msgs/msg/float32.hpp>

r2s::Node node("servo");

#if defined(ESP32)
#define SERVO_PIN 13
#elif defined(__AVR_ATmega2560__)
#define SERVO_PIN 11
#else
#define SERVO_PIN 9
#endif

void on_servo(const std_msgs::msg::Float32 &msg)
{
    float angle = msg.data;

    if (angle < 0.0f) {
        angle = 0.0f;
    }
    if (angle > 180.0f) {
        angle = 180.0f;
    }
#if defined(ESP32)
    // 12-bit LEDC at 50 Hz: duty 205 at 0 degrees to 410 at 180.
    ledcWrite(SERVO_PIN, (uint32_t)(205.0f + angle * (205.0f / 180.0f)));
#else
    OCR1A = (uint16_t)(2000.0f + angle * (2000.0f / 180.0f));
#endif
}
r2s::Subscription<std_msgs::msg::Float32> servo_sub(node, "servo", on_servo);

void setup()
{
    pinMode(SERVO_PIN, OUTPUT);
#if defined(ESP32)
    // 50 Hz LEDC at 12 bits; the duty math lives in on_servo.
    ledcAttach(SERVO_PIN, 50, 12);
#else
    // Phase and frequency correct PWM at 50 Hz: 16 MHz / 8 / 40000 ticks.
    TCCR1A = (uint8_t)((1u << COM1A1) | (1u << WGM11));
    TCCR1B = (uint8_t)((1u << WGM13) | (1u << WGM12) | (1u << CS11));
    ICR1 = 40000u;
    OCR1A = 3000u;
#endif
    node.begin(Serial, 115200);
}
void loop() { node.spin_some(); }
