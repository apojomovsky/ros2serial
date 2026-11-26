// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Param firmware for the simavr harness: one topic and one float64
 * parameter, announced over UART0 on a build whose double is 32 bits. The
 * parameter travels as binary64 ([PAR-3]): the loop reports the stored
 * binary32 bits after every SET that changes them, as a raw V and the 4
 * little-endian bytes after the framed STATUS, so the test sees the
 * narrowing the store applied. */

#include <avr/io.h>
#include <stdint.h>
#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_arch.h"
#include "r2s/r2s_params.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"

#define BAUD 115200UL
#define UBRR_VALUE (F_CPU / 16UL / BAUD - 1UL)

static const r2s_topic_t R2S_FLASH param_topics[] = {
  {16u, 0x00u, {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u}, "std_msgs/msg/Float32",
    "battery"},
};

static r2s_param_t param_table[] = {
  {"gain", R2S_PARAM_F64, 0u, 0u, {.f = 1.5}, {.f = 0}},
};

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t n = 0u;

  while (n < max && (UCSR0A & (1u << RXC0)) != 0u) {
    buf[n] = UDR0;
    n++;
  }
  return n;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    loop_until_bit_is_set(UCSR0A, UDRE0);
    UDR0 = buf[i];
  }
}

uint16_t r2s_port_tx_free(void)
{
  return 0xFFFFu;
}

/* The test drives every reply from its own frames, so a fixed tick keeps
 * the exchange deterministic: only solicited answers, never a repeat. */
uint32_t r2s_port_tick(void)
{
  return 0u;
}

int main(void)
{
  /* The default's bits, so only a SET the session stored is reported. */
  uint32_t last_bits = 0x3FC00000u;

  UBRR0 = (uint16_t)UBRR_VALUE;
  UCSR0B = (1 << RXEN0) | (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
  (void)r2s_session_configure("paramgain", param_topics, 1u);
  (void)r2s_params_configure(param_table, 1u);
  r2s_init();
  for (;; ) {
    const r2s_param_t *gain;
    uint32_t bits = 0u;
    uint8_t report[5];
    uint8_t i;

    r2s_spin_some();
    gain = r2s_params_get(0u);
    if (gain != NULL) {
      memcpy(&bits, &gain->value.f, sizeof bits);
    }
    if (bits != last_bits) {
      last_bits = bits;
      report[0] = 'V';
      for (i = 0u; i < 4u; i++) {
        report[1u + i] = (uint8_t)(bits >> (i * 8u));
      }
      r2s_port_write(report, (uint16_t)sizeof report);
    }
  }
}
