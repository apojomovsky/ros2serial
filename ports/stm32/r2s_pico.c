// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Pico glue: UART0 on GP0/GP1 plus a 1 ms SysTick behind the port
 * contract. The SDK boot stage leaves the clocks at 125 MHz sys and
 * peri, which the baud divisor below assumes. Register map: the RP2040
 * datasheet. Product page:
 * https://www.raspberrypi.com/documentation/microcontrollers/rp2040.html
 */
#include <stdint.h>

#include "r2s/r2s_port.h"

#include "r2s_board.h"

#define RESETS_BASE 0x4000C000u
#define RESETS_RESET (*(volatile uint32_t *)(RESETS_BASE + 0x00u))
#define RESETS_DONE (*(volatile uint32_t *)(RESETS_BASE + 0x08u))
#define RESETS_UART0 (1u << 22u)
#define RESETS_IO_BANK0 (1u << 5u)

#define IO_BANK0_BASE 0x40014000u
#define IO_BANK0_GPIO0_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x04u))
#define IO_BANK0_GPIO1_CTRL (*(volatile uint32_t *)(IO_BANK0_BASE + 0x0Cu))
/* Function 2 on both pins is UART0. */
#define IO_BANK0_FUNCSEL_UART 2u

#define PADS_BANK0_BASE 0x4001C000u
#define PADS_BANK0_GPIO0 (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x04u))
#define PADS_BANK0_GPIO1 (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x08u))
#define PADS_BANK0_IE (1u << 6u)

#define UART0_BASE 0x40034000u
#define UART0_DR (*(volatile uint32_t *)(UART0_BASE + 0x00u))
#define UART0_FR (*(volatile uint32_t *)(UART0_BASE + 0x18u))
#define UART0_IBRD (*(volatile uint32_t *)(UART0_BASE + 0x24u))
#define UART0_FBRD (*(volatile uint32_t *)(UART0_BASE + 0x28u))
#define UART0_LCR (*(volatile uint32_t *)(UART0_BASE + 0x2Cu))
#define UART0_CR (*(volatile uint32_t *)(UART0_BASE + 0x30u))
#define UART_FR_TXFF (1u << 5u)
#define UART_FR_RXFE (1u << 4u)
#define UART_LCR_WLEN_8 (3u << 5u)
#define UART_LCR_FEN (1u << 4u)
#define UART_CR_UARTEN (1u << 0u)
#define UART_CR_TXE (1u << 8u)
#define UART_CR_RXE (1u << 9u)
/* 125 MHz over 16 times 115200 gives 67.82: 67 and 52 sixtieths. */
#define UART0_IBRD_VALUE 67u
#define UART0_FBRD_VALUE 52u

#define SYSTICK_CTRL (*(volatile uint32_t *)0xE000E010u)
#define SYSTICK_LOAD (*(volatile uint32_t *)0xE000E014u)
#define SYSTICK_ENABLE (1u << 0u)
#define SYSTICK_TICKINT (1u << 1u)
#define SYSTICK_CLKSOURCE (1u << 2u)
/* 125 MHz needs 125000 ticks per millisecond. */
#define SYSTICK_RELOAD 124999u

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

/* The core exceptions plus the 26 RP2040 device interrupts. */
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
};

/* Releases UART0 and the pin bank from reset, bounded by a counter so a
 * stuck peripheral fails into the debugger rather than hanging boot. */
static void release_resets(void)
{
  uint32_t remaining = 1000000u;

  RESETS_RESET &= (uint32_t) ~(RESETS_UART0 | RESETS_IO_BANK0);
  while (((RESETS_DONE & (RESETS_UART0 | RESETS_IO_BANK0)) !=
    (RESETS_UART0 | RESETS_IO_BANK0)) &&
    remaining > 0u)
  {
    remaining--;
  }
}

/* Starts UART0 at 115200 8-N-1 on GP0/GP1 and the millisecond tick. */
void r2s_board_init(void)
{
  release_resets();
  IO_BANK0_GPIO0_CTRL = IO_BANK0_FUNCSEL_UART;
  IO_BANK0_GPIO1_CTRL = IO_BANK0_FUNCSEL_UART;
  PADS_BANK0_GPIO0 = PADS_BANK0_IE;
  PADS_BANK0_GPIO1 = PADS_BANK0_IE;
  UART0_CR = 0u;
  UART0_IBRD = UART0_IBRD_VALUE;
  UART0_FBRD = UART0_FBRD_VALUE;
  UART0_LCR = (uint32_t)(UART_LCR_WLEN_8 | UART_LCR_FEN);
  UART0_CR = (uint32_t)(UART_CR_UARTEN | UART_CR_TXE | UART_CR_RXE);
  SYSTICK_LOAD = SYSTICK_RELOAD;
  SYSTICK_CTRL = (uint32_t)(SYSTICK_ENABLE | SYSTICK_TICKINT | SYSTICK_CLKSOURCE);
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t count = 0u;

  while (count < max && (UART0_FR & UART_FR_RXFE) == 0u) {
    buf[count] = (uint8_t)UART0_DR;
    count++;
  }
  return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    while ((UART0_FR & UART_FR_TXFF) != 0u) {
    }
    UART0_DR = buf[i];
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
