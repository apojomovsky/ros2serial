// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* ch32v port: the core's port contract over USART1 (PD5/PD6) with the
 * SysTick as the millisecond source. Polling only, so no trap setup. */
#include <stdint.h>

#include "r2s/r2s_port.h"

#include "r2s_ch32v_port.h"
#include "r2s_ch32v_regs.h"

#ifdef R2S_CH32V_HOST_TEST
r2s_ch32v_rcc_t R2S_CH32V_TEST_rcc;
r2s_ch32v_gpio_t R2S_CH32V_TEST_gpiod;
r2s_ch32v_usart_t R2S_CH32V_TEST_usart1;
r2s_ch32v_stk_t R2S_CH32V_TEST_stk;
uint8_t R2S_CH32V_TEST_tx[256];
uint16_t R2S_CH32V_TEST_tx_count;

void r2s_ch32v_test_regs_reset(void)
{
  r2s_ch32v_rcc_t rcc = {0};
  r2s_ch32v_gpio_t gpiod = {0};
  r2s_ch32v_usart_t usart1 = {0};
  r2s_ch32v_stk_t stk = {0};
  uint16_t i;

  R2S_CH32V_TEST_rcc = rcc;
  R2S_CH32V_TEST_gpiod = gpiod;
  R2S_CH32V_TEST_usart1 = usart1;
  R2S_CH32V_TEST_stk = stk;
  /* The host has no clock tree, so the init waits see settled hardware. */
  R2S_CH32V_TEST_rcc.CTLR = R2S_CH32V_RCC_CTLR_PLLRDY;
  R2S_CH32V_TEST_rcc.CFGR0 = R2S_CH32V_RCC_CFGR0_SWS_PLL;
  for (i = 0u; i < sizeof(R2S_CH32V_TEST_tx); i++) {
    R2S_CH32V_TEST_tx[i] = 0u;
  }
  R2S_CH32V_TEST_tx_count = 0u;
}
#endif

void r2s_ch32v_init(uint32_t hclk_hz, uint32_t baud)
{
  uint32_t cfglr;

  /* The writes below OR into the control registers instead of assigning
   * them: reset CFGR0 is zero and the OR keeps the factory trim and the
   * host-test ready flags, so the target sees the same bits either way. */
  R2S_CH32V_RCC->CFGR0 |=
    (R2S_CH32V_RCC_CFGR0_HPRE_DIV1 | R2S_CH32V_RCC_CFGR0_PLLSRC_HSI_MUL2);
  R2S_CH32V_RCC->CTLR |= (R2S_CH32V_RCC_CTLR_HSITRIM | R2S_CH32V_RCC_CTLR_HSION |
    R2S_CH32V_RCC_CTLR_PLLON);
  while ((R2S_CH32V_RCC->CTLR & R2S_CH32V_RCC_CTLR_PLLRDY) == 0u) {
  }
  R2S_CH32V_RCC->CFGR0 |= R2S_CH32V_RCC_CFGR0_SW_PLL;
  while ((R2S_CH32V_RCC->CFGR0 & R2S_CH32V_RCC_CFGR0_SWS) !=
    R2S_CH32V_RCC_CFGR0_SWS_PLL)
  {
  }
  R2S_CH32V_RCC->INTR = R2S_CH32V_RCC_INTR_CLEAR;

  /* GPIOD for the pins and USART1 itself leave reset together. */
  R2S_CH32V_RCC->APB2PCENR |= (R2S_CH32V_RCC_GPIODEN | R2S_CH32V_RCC_USART1EN);

  /* PD5 alternate push-pull, PD6 floating input: the default mapping. */
  cfglr = R2S_CH32V_GPIOD->CFGLR;
  cfglr &= ~((uint32_t)(0xFu << (4u * R2S_CH32V_TX_PIN)));
  cfglr |= (uint32_t)(R2S_CH32V_GPIO_TX_AF_PP << (4u * R2S_CH32V_TX_PIN));
  cfglr &= ~((uint32_t)(0xFu << (4u * R2S_CH32V_RX_PIN)));
  cfglr |= (uint32_t)(R2S_CH32V_GPIO_RX_FLOAT << (4u * R2S_CH32V_RX_PIN));
  R2S_CH32V_GPIOD->CFGLR = cfglr;

  /* 8-N-1 with transmitter and receiver on, then the divisor. */
  R2S_CH32V_USART1->CTLR1 =
    (R2S_CH32V_USART_TE | R2S_CH32V_USART_RE);
  R2S_CH32V_USART1->CTLR2 = 0u;
  R2S_CH32V_USART1->CTLR3 = 0u;
  R2S_CH32V_USART1->BRR = r2s_ch32v_brr(hclk_hz, baud);
  R2S_CH32V_USART1->CTLR1 |= R2S_CH32V_USART_UE;

  /* Free-running at HCLK/8: the tick reads CNT with no interrupt. */
  R2S_CH32V_STK->CTLR = R2S_CH32V_STK_STE;
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t count = 0u;

  while ((count < max) && ((R2S_CH32V_USART1->STATR & R2S_CH32V_USART_RXNE) != 0u)) {
    buf[count] = r2s_ch32v_usart_recv(R2S_CH32V_USART1);
    count++;
  }
  return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    while ((R2S_CH32V_USART1->STATR & R2S_CH32V_USART_TXE) == 0u) {
    }
    r2s_ch32v_usart_send(R2S_CH32V_USART1, buf[i]);
  }
}

/* The write above polls for space, so it never refuses a byte. */
uint16_t r2s_port_tx_free(void)
{
  return 0xFFFFu;
}

uint32_t r2s_port_tick(void)
{
  return r2s_ch32v_ms(R2S_CH32V_STK->CNT, R2S_CH32V_CNT_PER_MS);
}
