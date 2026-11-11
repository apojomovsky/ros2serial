// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* The template glue's host test: the port contract against fake registers.
 * Fails unless the glue computes the USART divisor, converts the timer
 * count to milliseconds, enables the port on init, and moves bytes both
 * ways through the faked UART. */
#include <stdint.h>
#include <stdio.h>

#include "r2s/r2s_port.h"
#include "r2s_template_port.h"
#include "r2s_template_regs.h"

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("fail line %d: %s\n", __LINE__, #cond); \
      return 1; \
    } \
  } while (0)

static void reset_all(void)
{
  r2s_template_test_regs_reset();
}

static int check_brr_truncates(void)
{
  /* 48 MHz divided by 115200 leaves a fraction the divisor drops. */
  CHECK(r2s_template_brr(48000000u, 115200u) == 416u);
  CHECK(r2s_template_brr(24000000u, 115200u) == 208u);
  return 0;
}

static int check_brr_refuses_zero_baud(void)
{
  /* A zero baud would divide by zero, so the helper reports no divisor. */
  CHECK(r2s_template_brr(48000000u, 0u) == 0u);
  return 0;
}

static int check_ms_converts_and_wraps(void)
{
  /* 1 count makes a millisecond at the 1 kHz timer clock. */
  CHECK(r2s_template_ms(0u, 1u) == 0u);
  CHECK(r2s_template_ms(1u, 1u) == 1u);
  /* A full 32-bit count wraps the same way the tick the core reads does. */
  CHECK(r2s_template_ms(0xFFFFFFFFu, 1u) == 0xFFFFFFFFu);
  /* A zero rate would divide by zero, so the helper reports 0 instead. */
  CHECK(r2s_template_ms(1u, 0u) == 0u);
  return 0;
}

static int check_init_enables_uart(void)
{
  reset_all();
  r2s_template_init(48000000u, 115200u);
  /* The divisor matches the 48 MHz clock the init is told about. */
  CHECK(R2S_TEMPLATE_TEST_uart.BRR == 416u);
  /* Transmitter, receiver and the UART leave reset together. */
  CHECK((R2S_TEMPLATE_TEST_uart.CTL & R2S_TEMPLATE_CTL_UE) != 0u);
  CHECK((R2S_TEMPLATE_TEST_uart.CTL & R2S_TEMPLATE_CTL_TE) != 0u);
  CHECK((R2S_TEMPLATE_TEST_uart.CTL & R2S_TEMPLATE_CTL_RE) != 0u);
  return 0;
}

static int check_write_sends_bytes_in_order(void)
{
  const uint8_t out[] = {'o', 'k'};

  reset_all();
  R2S_TEMPLATE_TEST_uart.STAT = R2S_TEMPLATE_STAT_TXE;
  r2s_port_write(out, sizeof out);
  CHECK(R2S_TEMPLATE_TEST_tx_count == 2u);
  CHECK(R2S_TEMPLATE_TEST_tx[0] == (uint8_t)'o');
  CHECK(R2S_TEMPLATE_TEST_tx[1] == (uint8_t)'k');
  return 0;
}

static int check_read_stops_at_empty(void)
{
  uint8_t buf[8];

  reset_all();
  R2S_TEMPLATE_TEST_uart.DATA = (uint32_t)'a';
  R2S_TEMPLATE_TEST_uart.STAT = R2S_TEMPLATE_STAT_RXNE;
  CHECK(r2s_port_read(buf, sizeof buf) == 1u);
  CHECK(buf[0] == (uint8_t)'a');
  CHECK(r2s_port_read(buf, sizeof buf) == 0u);
  CHECK(r2s_port_read(buf, 0u) == 0u);
  return 0;
}

static int check_tick_reads_the_timer(void)
{
  reset_all();
  R2S_TEMPLATE_TEST_timer.CNT = 60000u;
  CHECK(r2s_port_tick() == 60000u);
  R2S_TEMPLATE_TEST_timer.CNT = 0u;
  CHECK(r2s_port_tick() == 0u);
  /* The tick wraps at 2^32, the way r2s_port.h promises. */
  R2S_TEMPLATE_TEST_timer.CNT = 0xFFFFFFFFu;
  CHECK(r2s_port_tick() == 0xFFFFFFFFu);
  return 0;
}

static int check_init_starts_the_tick(void)
{
  reset_all();
  r2s_template_init(48000000u, 115200u);
  /* The tick source runs free from init, which the port claims. */
  CHECK((R2S_TEMPLATE_TEST_timer.CTL & R2S_TEMPLATE_TIMER_STE) != 0u);
  CHECK(r2s_port_tick() == 0u);
  return 0;
}

static int check_tx_reports_blocking(void)
{
  /* The polled write never refuses a byte, like the bluepill firmware's. */
  CHECK(r2s_port_tx_free() == 0xFFFFu);
  return 0;
}

int main(void)
{
  int failures = 0;

  failures += check_brr_truncates();
  failures += check_brr_refuses_zero_baud();
  failures += check_ms_converts_and_wraps();
  failures += check_init_enables_uart();
  failures += check_write_sends_bytes_in_order();
  failures += check_read_stops_at_empty();
  failures += check_tick_reads_the_timer();
  failures += check_init_starts_the_tick();
  failures += check_tx_reports_blocking();
  if (failures != 0) {
    printf("%d template checks failed\n", failures);
    return 1;
  }
  printf("template port: all checks passed\n");
  return 0;
}
