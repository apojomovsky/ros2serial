// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* PIC18F4550 sim firmware: the generated pair on EUSART1, publishing one
 * Float32 and counting cmd_vel frames. The topic table matches the frozen
 * handshake vector, so the simulator's capture compares to it byte for
 * byte. */
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s_generated.h"

#include "r2s_pic18f4550_port.h"

/* The part's clock, the same value the sim run programs into the
 * simulator's oscillator property. */
#define R2S_PIC18F4550_CLK_HZ 20000000u

/* The node name ports/epic-hal/r2s.yaml declares; the firmware spells it,
 * as every other example does, so the config and this line are the pair
 * the HELLO announces. */
#define R2S_SIM_NODE_NAME "demo"

/* The transcript answers READY at 350 ms and closes on the second host
 * heartbeat at 1350 ms, so the first publish waits past that: the capture
 * is compared frame by frame to the vector, and a user frame inside the
 * window would offset every reply after it. */
#define R2S_SIM_FIRST_PUBLISH_MS 1500u
#define R2S_SIM_PUBLISH_PERIOD_MS 100u
#define R2S_SIM_MS_TO_TICKS(ms) ((uint32_t)(ms) * (uint32_t)R2S_TICK_HZ / 1000u)

static uint32_t s_cmd_vel_count;

/* Handles cmd_vel; dispatch calls it from r2s_spin_some(). */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
  (void)msg;
  s_cmd_vel_count++;
}

int main(void)
{
  std_msgs__msg__Float32 battery;
  uint32_t last_pub;

  r2s_pic18f4550_init(R2S_PIC18F4550_CLK_HZ, 115200u);
  if (r2s_session_configure(R2S_SIM_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
    return 1;
  }
  r2s_init();
  battery.data = 1.5f;
  last_pub = r2s_port_tick();
  for (;; ) {
    uint32_t now;

    r2s_spin_some();
    now = r2s_port_tick();
    /* One publish per period once the transcript's window has closed, so
     * the run shows a user frame going out without disturbing the
     * handshake the vector pins. */
    if (r2s_connected() == 0u || (uint32_t)(now) < R2S_SIM_MS_TO_TICKS(R2S_SIM_FIRST_PUBLISH_MS)) {
      last_pub = now;
      continue;
    }
    if ((uint32_t)(now - last_pub) >= R2S_SIM_MS_TO_TICKS(R2S_SIM_PUBLISH_PERIOD_MS)) {
      if (r2s_publish_battery(&battery) == R2S_OK) {
        last_pub += R2S_SIM_MS_TO_TICKS(R2S_SIM_PUBLISH_PERIOD_MS);
        if ((uint32_t)(now - last_pub) >= R2S_SIM_MS_TO_TICKS(R2S_SIM_PUBLISH_PERIOD_MS)) {
          last_pub = now;
        }
      }
    }
  }
}
