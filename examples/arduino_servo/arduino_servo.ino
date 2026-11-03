/* Servo on the runtime API: a Float32 subscription on servo sets the angle.
 * Timer1 drives the 50 Hz pulse a hobby servo expects, 1 ms at 0 degrees
 * to 2 ms at 180, so the link never blocks for the pulse. OC1A is pin 9 on
 * the Uno and pin 11 on the Mega. Taking Timer1 stops analogWrite on its
 * two pins.
 * Try it with: ros2 topic pub /servo std_msgs/msg/Float32 "{data: 90.0}" --once
 * (bridge launch as in the README walkthrough). */

#include <ros2serial.h>
#include <std_msgs/msg/float32.hpp>

r2s::Node node("servo");

#if defined(__AVR_ATmega2560__)
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
    OCR1A = (uint16_t)(2000.0f + angle * (2000.0f / 180.0f));
}
r2s::Subscription<std_msgs::msg::Float32> servo_sub(node, "servo", on_servo);

void setup()
{
    pinMode(SERVO_PIN, OUTPUT);
    // Phase and frequency correct PWM at 50 Hz: 16 MHz / 8 / 40000 ticks.
    TCCR1A = (uint8_t)((1u << COM1A1) | (1u << WGM11));
    TCCR1B = (uint8_t)((1u << WGM13) | (1u << WGM12) | (1u << CS11));
    ICR1 = 40000u;
    OCR1A = 3000u;
    node.begin(Serial, 115200);
}
void loop() { node.spin_some(); }
