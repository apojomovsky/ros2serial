// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Minimal STM32/RP2040 firmware: the generated pair on bare metal,
 * publishing battery and counting cmd_vel frames. The board setup lives
 * in the port glue behind r2s_board.h, so this file never names a
 * peripheral and builds for both targets unchanged. */
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s_board.h"
#include "r2s_generated.h"

static uint32_t s_cmd_vel_count;

/* Handles cmd_vel; dispatch calls it from r2s_spin_some(). */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
  (void)msg;
  s_cmd_vel_count++;
}

static uint32_t s_last_pub;
static uint32_t s_sent;

/* One loop step, broken out so a host test drives it directly. */
void loop_once(void);

/* An example publishes a fresh reading, so a missed interval is dropped,
 * not replayed: the deadline moves one period per publish and resyncs to
 * now when more than one period late, and the offline branch resets it
 * the same way. A stall then skips its frames instead of bursting them. */
void loop_once(void)
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

int main(void)
{
  r2s_board_init();
  if (r2s_session_configure("hello_stm32", r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
    return 1;
  }
  r2s_init();
  s_last_pub = r2s_port_tick();
  for (;; ) {
    loop_once();
  }
}
