// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Custom-type X-macro example: the interface package's own message through
 * the X-macro list on Uno and Mega. `r2s-gen types` emits the library beside
 * the sketch from `custom_interfaces/msg/SensorReading`, so the row names a
 * custom type exactly the way a bundled one is named, and no `r2s.yaml` or
 * generated topic table is involved. The counter travels in status and the
 * temperature derives from it, so a reader proves it decoded both fields. */

#define R2S_NODE_NAME "custom_xmacro"
#define R2S_TOPICS(X) \
    X(Publisher, custom_interfaces::msg::SensorReading, reading_pub, "reading")
#include <ros2serial.h>

static uint16_t s_status;

void setup(void)
{
    Serial.begin(115200);
    if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return;
    }
    r2s_init();
}

/* An example publishes a fresh reading, so a missed interval is dropped,
 * not replayed: the deadline moves one period per publish and resyncs to
 * now when more than one period late, and the offline branch resets it
 * the same way, which is the pattern examples/arduino_xmacro keeps. */
void loop(void)
{
    static uint32_t last_pub;
    uint32_t now;

    r2s_spin_some();
    now = r2s_port_tick();
    if (r2s_connected() == 0u) {
        last_pub = now;
        return;
    }
    if ((uint32_t)(now - last_pub) >= 100u) {
        custom_interfaces::msg::SensorReading reading = {};

        reading.temperature = (float)s_status / 10.0f;
        reading.status = s_status;
        if (r2s_publish_reading_pub(&reading) == R2S_OK) {
            s_status++;
            last_pub += 100u;
            if ((uint32_t)(now - last_pub) >= 100u) {
                last_pub = now;
            }
        }
    }
}
