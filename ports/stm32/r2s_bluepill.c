// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Blue Pill glue: USART1 plus a 1 ms SysTick behind the port contract.
 * Runs on the 8 MHz internal oscillator, so no crystal or PLL setup is
 * involved; the baud divisor error stays under a percent, which the
 * bridge tolerates. Register map: RM0008. Product page:
 * https://www.st.com/en/microcontrollers-microprocessors/stm32f103c8.html
 */
#include <stdint.h>

#include "r2s/r2s_port.h"

#include "r2s_board.h"

#define RCC_BASE 0x40021000u
#define RCC_APB2ENR (*(volatile uint32_t *)(RCC_BASE + 0x18u))
#define RCC_APB2ENR_IOPAEN (1u << 2u)
#define RCC_APB2ENR_USART1EN (1u << 14u)

#define GPIOA_BASE 0x40010800u
#define GPIOA_CRL (*(volatile uint32_t *)(GPIOA_BASE + 0x00u))
#define GPIOA_CRL_PA9_SHIFT 4u
#define GPIOA_CRL_PA10_SHIFT 8u
/* PA9 alternate push-pull at 50 MHz; PA10 floating input. */
#define GPIOA_CRL_PA9_ALT_PP 0xBu
#define GPIOA_CRL_PA10_FLOAT 0x4u
#define GPIOA_CRL_NIBBLE_MASK 0xFu

#define USART1_BASE 0x40013800u
#define USART1_SR (*(volatile uint32_t *)(USART1_BASE + 0x00u))
#define USART1_DR (*(volatile uint32_t *)(USART1_BASE + 0x04u))
#define USART1_BRR (*(volatile uint32_t *)(USART1_BASE + 0x08u))
#define USART1_CR1 (*(volatile uint32_t *)(USART1_BASE + 0x0Cu))
#define USART_SR_TXE (1u << 7u)
#define USART_SR_RXNE (1u << 5u)
#define USART_CR1_UE (1u << 13u)
#define USART_CR1_TE (1u << 3u)
#define USART_CR1_RE (1u << 2u)
/* 8 MHz over 16 times 115200 gives 4.34: mantissa 4, fraction 5. */
#define USART1_BRR_VALUE ((4u << 4u) | 5u)

#define SYSTICK_CTRL (*(volatile uint32_t *)0xE000E010u)
#define SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014u)
#define SYSTICK_ENABLE (1u << 0u)
#define SYSTICK_TICKINT (1u << 1u)
/* The internal clock over 8 is 1 MHz, so 1000 ticks make a millisecond. */
#define SYSTICK_RELOAD 999u

/* The stack top is an address, and the table takes it as one: naming it
 * as a function designator avoids the object-to-function cast. */
extern void _estack(void);
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

int main(void);

static volatile uint32_t s_ms;

void Reset_Handler(void);
void Default_Handler(void);
void SysTick_Handler(void);

/* Faults and unused interrupts wait here for the debugger. */
void Default_Handler(void)
{
  for (;; ) {
  }
}

/* Milliseconds since boot: a 32-bit aligned access is atomic on this
 * core, so the reader needs no lock. */
void SysTick_Handler(void)
{
  s_ms++;
}

/* Copies .data from flash, zeroes .bss, then runs the firmware. */
void Reset_Handler(void)
{
  uint32_t *src = &_sidata;
  uint32_t *dst = &_sdata;

  while (dst < &_edata) {
    *dst = *src;
    dst++;
    src++;
  }
  dst = &_sbss;
  while (dst < &_ebss) {
    *dst = 0u;
    dst++;
  }
  main();
  for (;; ) {
  }
}

/* The core exceptions plus the 43 medium-density device interrupts. */
__attribute__((section(".isr_vector"))) void (*const s_vectors[])(void) = {
  _estack,
  Reset_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  SysTick_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
  Default_Handler,
};

/* Starts USART1 at 115200 8-N-1 on PA9/PA10 and the millisecond tick. */
void r2s_board_init(void)
{
  uint32_t crl;

  RCC_APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_USART1EN;
  crl = GPIOA_CRL;
  crl &= ~((uint32_t)(GPIOA_CRL_NIBBLE_MASK << GPIOA_CRL_PA9_SHIFT) |
    (uint32_t)(GPIOA_CRL_NIBBLE_MASK << GPIOA_CRL_PA10_SHIFT));
  crl |= (uint32_t)(GPIOA_CRL_PA9_ALT_PP << GPIOA_CRL_PA9_SHIFT) |
    (uint32_t)(GPIOA_CRL_PA10_FLOAT << GPIOA_CRL_PA10_SHIFT);
  GPIOA_CRL = crl;
  USART1_BRR = USART1_BRR_VALUE;
  USART1_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
  SYSTICK_LOAD = SYSTICK_RELOAD;
  SYSTICK_CTRL = SYSTICK_ENABLE | SYSTICK_TICKINT;
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t count = 0u;

  while (count < max && (USART1_SR & USART_SR_RXNE) != 0u) {
    buf[count] = (uint8_t)USART1_DR;
    count++;
  }
  return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    while ((USART1_SR & USART_SR_TXE) == 0u) {
    }
    USART1_DR = buf[i];
  }
}

/* The write above polls for space, so it never refuses a byte. */
uint16_t r2s_port_tx_free(void)
{
  return 0xFFFFu;
}

uint32_t r2s_port_tick(void)
{
  return s_ms;
}
