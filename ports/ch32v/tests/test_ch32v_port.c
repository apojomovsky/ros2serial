// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* The CH32V00x glue's host test: the port contract against fake registers.
 * Fails unless the glue computes the USART divisor, converts the SysTick
 * count to milliseconds, enables the port on init, and moves bytes both
 * ways through the faked USART. */
#include <stdint.h>
#include <stdio.h>

#include "r2s/r2s_port.h"
#include "r2s_ch32v_port.h"
#include "r2s_ch32v_regs.h"

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("fail line %d: %s\n", __LINE__, #cond); \
      return 1; \
    } \
  } while (0)

static void reset_all(void)
{
  r2s_ch32v_test_regs_reset();
}

static int check_brr_truncates(void)
{
  /* 48 MHz divided by 115200 leaves a fraction the divisor drops. */
  CHECK(r2s_ch32v_brr(48000000u, 115200u) == 416u);
  CHECK(r2s_ch32v_brr(24000000u, 115200u) == 208u);
  return 0;
}

static int check_brr_refuses_zero_baud(void)
{
  /* A zero baud would divide by zero, so the helper reports no divisor. */
  CHECK(r2s_ch32v_brr(48000000u, 0u) == 0u);
  return 0;
}

static int check_ms_converts_and_wraps(void)
{
  /* 6 MHz SysTick counts (HCLK/8 at 48 MHz) make 6000 counts a millisecond. */
  CHECK(r2s_ch32v_ms(0u, 6000u) == 0u);
  CHECK(r2s_ch32v_ms(6000u, 6000u) == 1u);
  CHECK(r2s_ch32v_ms(5999u, 6000u) == 0u);
  /* A full 32-bit count wraps the same way the tick the core reads does. */
  CHECK(r2s_ch32v_ms(0xFFFFFFFFu, 6000u) == 715827u);
  return 0;
}

static int check_init_enables_clocks_and_usart(void)
{
  reset_all();
  r2s_ch32v_init(48000000u, 115200u);
  /* GPIOD for the pins and USART1 itself leave reset together. */
  CHECK((R2S_CH32V_TEST_rcc.APB2PCENR & R2S_CH32V_RCC_GPIODEN) != 0u);
  CHECK((R2S_CH32V_TEST_rcc.APB2PCENR & R2S_CH32V_RCC_USART1EN) != 0u);
  /* The divisor matches the 48 MHz clock the init switches on. */
  CHECK(R2S_CH32V_TEST_usart1.BRR == 416u);
  /* Transmitter, receiver and the USART leave reset together. */
  CHECK((R2S_CH32V_TEST_usart1.CTLR1 & R2S_CH32V_USART_UE) != 0u);
  CHECK((R2S_CH32V_TEST_usart1.CTLR1 & R2S_CH32V_USART_TE) != 0u);
  CHECK((R2S_CH32V_TEST_usart1.CTLR1 & R2S_CH32V_USART_RE) != 0u);
  /* The SysTick runs free without its interrupt, which the port never uses. */
  CHECK(R2S_CH32V_TEST_stk.CTLR == R2S_CH32V_STK_STE);
  return 0;
}

static int check_init_maps_pd5_pd6(void)
{
  reset_all();
  r2s_ch32v_init(48000000u, 115200u);
  /* PD5 (TX) is alternate push-pull, PD6 (RX) floats: the default mapping. */
  CHECK(((R2S_CH32V_TEST_gpiod.CFGLR >> (4u * 5u)) & 0xFu) == 0xBu);
  CHECK(((R2S_CH32V_TEST_gpiod.CFGLR >> (4u * 6u)) & 0xFu) == 0x4u);
  return 0;
}

static int check_write_sends_bytes_in_order(void)
{
  const uint8_t out[] = {'o', 'k'};

  reset_all();
  R2S_CH32V_TEST_usart1.STATR = R2S_CH32V_USART_TXE;
  r2s_port_write(out, sizeof out);
  CHECK(R2S_CH32V_TEST_tx_count == 2u);
  CHECK(R2S_CH32V_TEST_tx[0] == (uint8_t)'o');
  CHECK(R2S_CH32V_TEST_tx[1] == (uint8_t)'k');
  return 0;
}

static int check_read_stops_at_empty(void)
{
  uint8_t buf[8];

  reset_all();
  R2S_CH32V_TEST_usart1.DATAR = (uint32_t)'a';
  R2S_CH32V_TEST_usart1.STATR = R2S_CH32V_USART_RXNE;
  CHECK(r2s_port_read(buf, sizeof buf) == 1u);
  CHECK(buf[0] == (uint8_t)'a');
  R2S_CH32V_TEST_usart1.STATR = 0u;
  CHECK(r2s_port_read(buf, sizeof buf) == 0u);
  CHECK(r2s_port_read(buf, 0u) == 0u);
  return 0;
}

static int check_tick_reads_the_systick(void)
{
  reset_all();
  R2S_CH32V_TEST_stk.CNT = 60000u;
  CHECK(r2s_port_tick() == 10u);
  R2S_CH32V_TEST_stk.CNT = 0u;
  CHECK(r2s_port_tick() == 0u);
  return 0;
}

static int check_tx_reports_blocking(void)
{
  /* The polled write never refuses a byte, like the AVR firmware's. */
  CHECK(r2s_port_tx_free() == 0xFFFFu);
  return 0;
}

int main(void)
{
  int failures = 0;

  failures += check_brr_truncates();
  failures += check_brr_refuses_zero_baud();
  failures += check_ms_converts_and_wraps();
  failures += check_init_enables_clocks_and_usart();
  failures += check_init_maps_pd5_pd6();
  failures += check_write_sends_bytes_in_order();
  failures += check_read_stops_at_empty();
  failures += check_tick_reads_the_systick();
  failures += check_tx_reports_blocking();
  if (failures != 0) {
    printf("%d checks failed\n", failures);
    return 1;
  }
  printf("ch32v port ok\n");
  return 0;
}
