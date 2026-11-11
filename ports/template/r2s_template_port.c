// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Template port: the core's port contract over one UART with a
 * free-running counter as the millisecond source. Polling only, so no
 * interrupt setup is involved. */
#include <stdint.h>

#include "r2s/r2s_port.h"

#include "r2s_template_port.h"
#include "r2s_template_regs.h"

/* Counts per millisecond at the 1 MHz timer clock the init assumes. */
#define R2S_TEMPLATE_COUNTS_PER_MS 1000u

#ifdef R2S_TEMPLATE_HOST_TEST
r2s_template_uart_t R2S_TEMPLATE_TEST_uart;
r2s_template_timer_t R2S_TEMPLATE_TEST_timer;
uint8_t R2S_TEMPLATE_TEST_tx[256];
uint16_t R2S_TEMPLATE_TEST_tx_count;

void r2s_template_test_regs_reset(void)
{
  r2s_template_uart_t uart = {0};
  r2s_template_timer_t timer = {0};
  uint16_t i;

  R2S_TEMPLATE_TEST_uart = uart;
  R2S_TEMPLATE_TEST_timer = timer;
  for (i = 0u; i < sizeof(R2S_TEMPLATE_TEST_tx); i++) {
    R2S_TEMPLATE_TEST_tx[i] = 0u;
  }
  R2S_TEMPLATE_TEST_tx_count = 0u;
}
#endif

void r2s_template_init(uint32_t clk_hz, uint32_t baud)
{
  /* 8-N-1 with transmitter and receiver on, then the divisor. */
  R2S_TEMPLATE_UART->CTL = (R2S_TEMPLATE_CTL_UE | R2S_TEMPLATE_CTL_TE |
    R2S_TEMPLATE_CTL_RE);
  R2S_TEMPLATE_UART->BRR = r2s_template_brr(clk_hz, baud);

  /* Free-running: the tick reads CNT with no interrupt. */
  R2S_TEMPLATE_TIMER->CNT = 0u;
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t count = 0u;

  while ((count < max) && ((R2S_TEMPLATE_UART->STAT & R2S_TEMPLATE_STAT_RXNE) != 0u)) {
    buf[count] = r2s_template_uart_recv(R2S_TEMPLATE_UART);
    count++;
  }
  return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    while ((R2S_TEMPLATE_UART->STAT & R2S_TEMPLATE_STAT_TXE) == 0u) {
    }
    r2s_template_uart_send(R2S_TEMPLATE_UART, buf[i]);
  }
}

/* The write above polls for space, so it never refuses a byte. */
uint16_t r2s_port_tx_free(void)
{
  return 0xFFFFu;
}

uint32_t r2s_port_tick(void)
{
  return r2s_template_ms(R2S_TEMPLATE_TIMER->CNT, R2S_TEMPLATE_COUNTS_PER_MS);
}
