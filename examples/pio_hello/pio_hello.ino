// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Hello world on the generated PlatformIO layout: a Float32 publisher on
 * battery and a Twist subscriber on cmd_vel. The PlatformIO manifest
 * carries the frame limits as build flags, so the sketch names no defines.
 * Built by scripts/build-examples-pio.sh for Uno and Mega. */

#include <Arduino.h>

#include "r2s/r2s.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s_generated.h"

static uint32_t s_cmd_vel_count;
static uint32_t s_sent;
static uint32_t s_last_pub;

/* Handles cmd_vel; dispatch calls it from r2s_spin_some(). */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
  (void)msg;
  s_cmd_vel_count++;
}

/* An example publishes a fresh reading, so a missed interval is dropped,
 * not replayed: the deadline moves one period per publish and resyncs to
 * now when more than one period late, and the offline branch resets it
 * the same way. */
void setup(void)
{
  Serial.begin(115200);
  if (r2s_session_configure("hello_pio", r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
    return;
  }
  r2s_init();
  s_last_pub = r2s_port_tick();
}

void loop(void)
{
  uint32_t now;

  r2s_spin_some();
  now = r2s_port_tick();
  if (r2s_connected() == 0u) {
    s_last_pub = now;
    return;
  }
  if ((uint32_t)(now - s_last_pub) >= 100u) {
    std_msgs__msg__Float32 msg;

    msg.data = (float)s_sent;
    if (r2s_publish_battery(&msg) == R2S_OK) {
      s_sent++;
      s_last_pub += 100u;
      if ((uint32_t)(now - s_last_pub) >= 100u) {
        s_last_pub = now;
      }
    }
  }
}
