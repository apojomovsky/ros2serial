// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Power-up probe: the first r2s_init() without any reset must announce, behind
 * the one delimiter that closes a line the loader may have written on. It has
 * its own main, because the Unity setUp resets every module first. */
#include <stdio.h>

#include "fake_port.h"
#include "r2s/r2s.h"

int main(void)
{
  uint16_t len;
  const uint8_t *tx;

  fake_port_reset();
  r2s_init();
  tx = fake_port_tx(&len);
  if (len < 2u || tx[0] != 0x00u || tx[len - 1u] != 0x00u) {
    fprintf(stderr, "powerup: expected a flush before the HELLO frame\n");
    return 1;
  }
  if (fake_port_tx_frame_count() != 1u || r2s_connected() != 0u) {
    fprintf(stderr, "powerup: expected one HELLO while unconnected\n");
    return 1;
  }
  return 0;
}
