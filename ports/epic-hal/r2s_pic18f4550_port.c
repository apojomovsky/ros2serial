// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* PIC18F4550 port: EUSART1 bytes in and out, Timer1 as the tick. Polling
 * only: the core asks for a byte, the glue reads the receive register, so
 * no interrupt vector and no ring of its own. */
#include <stdint.h>

#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"

#include "r2s_pic18f4550_port.h"
#include "r2s_pic18f4550_regs.h"

/* The instruction clock is clk_hz / 4, and Timer1 runs at 1:1, so one
 * tick of the core's R2S_TICK_HZ is that many counts. At 20 MHz a
 * millisecond is 5000 counts and a microsecond is 5, both inside the
 * 16-bit timer; the 1:8 prescaler would round a microsecond to zero. */
#define R2S_PIC18F4550_INSTRUCTION_DIVISOR 4u

/* The 8-bit SPBRG with BRGH high and BRG16 low divides the clock by 16. */
#define R2S_PIC18F4550_SPBRG_DIVISOR 16u

#ifdef R2S_PIC18F4550_HOST_TEST
r2s_pic18f4550_regs_t R2S_PIC18F4550_TEST_regs;

void r2s_pic18f4550_test_regs_reset(void)
{
  r2s_pic18f4550_regs_t regs = {0};

  R2S_PIC18F4550_TEST_regs = regs;
}

void r2s_pic18f4550_test_rx_queue(const uint8_t *bytes, uint16_t count)
{
  uint16_t i;

  R2S_PIC18F4550_TEST_regs.rx_count = count;
  R2S_PIC18F4550_TEST_regs.rx_taken = 0u;
  for (i = 0u; i < count; i++) {
    R2S_PIC18F4550_TEST_regs.rx[i] = bytes[i];
  }
}
#endif

/* The tick the core reads, kept here because Timer1 is 16 bits: the
 * hardware wraps every 65536 counts, and a tick count would start over
 * with it. elapsed holds the counts since the last whole tick, so a wrap
 * costs nothing. */
static struct
{
  uint32_t count;
  uint16_t last;
  uint32_t elapsed;
  uint16_t per_tick;
} tick;

uint8_t r2s_pic18f4550_spbrg(uint32_t clk_hz, uint32_t baud)
{
  uint32_t divisor;

  if (baud == 0u) {
    return 0u;
  }
  divisor = clk_hz / (R2S_PIC18F4550_SPBRG_DIVISOR * baud);
  /* The register is 8 bits, so a divisor that does not fit reports 0
   * rather than wrapping into a wrong baud rate. */
  if (divisor == 0u || divisor > 255u) {
    return 0u;
  }
  return (uint8_t)(divisor - 1u);
}

uint16_t r2s_pic18f4550_cycles_per_tick(uint32_t clk_hz)
{
  uint32_t counts;

  if (clk_hz == 0u) {
    return 0u;
  }
  counts = clk_hz / (R2S_PIC18F4550_INSTRUCTION_DIVISOR * R2S_TICK_HZ);
  if (counts == 0u) {
    return 1u;
  }
  if (counts > 0xFFFFu) {
    return 0xFFFFu;
  }
  return (uint16_t)counts;
}

void r2s_pic18f4550_init(uint32_t clk_hz, uint32_t baud)
{
  /* RC6 drives the line and RC7 listens, so the UART owns both. */
  r2s_pic18f4550_trisc_write((uint8_t)((r2s_pic18f4550_trisc_read() |
    R2S_PIC18F4550_TRISC_RX) & (uint8_t) ~R2S_PIC18F4550_TRISC_TX));

  r2s_pic18f4550_spbrg_write(r2s_pic18f4550_spbrg(clk_hz, baud));
  /* Asynchronous, 8-bit, transmit on, high-speed divisor. */
  r2s_pic18f4550_txsta_write((uint8_t)(R2S_PIC18F4550_TXSTA_TXEN |
    R2S_PIC18F4550_TXSTA_BRGH));
  /* Serial port on, continuous receive on. */
  r2s_pic18f4550_rcsta_write((uint8_t)(R2S_PIC18F4550_RCSTA_SPEN |
    R2S_PIC18F4550_RCSTA_CREN));

  /* Timer1 from zero at 1:1, which is the rate the tick conversion
   * divides by. */
  r2s_pic18f4550_tmr1_write(0u);
  r2s_pic18f4550_t1con_write((uint8_t)(R2S_PIC18F4550_T1CON_TMR1ON |
    R2S_PIC18F4550_T1CON_T1_1_1));
  tick.count = 0u;
  tick.last = 0u;
  tick.elapsed = 0u;
  tick.per_tick = r2s_pic18f4550_cycles_per_tick(clk_hz);
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t count = 0u;

  while (count < max) {
    if (r2s_pic18f4550_rcif() == 0u) {
      break;
    }
    /* Reading the receive register clears RCIF. An overrun latches OERR,
     * and the receiver stays deaf until CREN is cycled, so clear it here
     * rather than lose every later byte. */
    if ((r2s_pic18f4550_rcsta_read() & R2S_PIC18F4550_RCSTA_OERR) != 0u) {
      r2s_pic18f4550_rcsta_write((uint8_t)(r2s_pic18f4550_rcsta_read() &
        (uint8_t) ~R2S_PIC18F4550_RCSTA_CREN));
      r2s_pic18f4550_rcsta_write((uint8_t)(r2s_pic18f4550_rcsta_read() |
        R2S_PIC18F4550_RCSTA_CREN));
    }
    buf[count] = r2s_pic18f4550_rcreg_read();
    count++;
  }
  return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    /* The core asks tx_free() before writing, but the register empties on
     * its own schedule, so the glue still waits for it: one byte at
     * 115200 costs under 90 us of polling. */
    while (r2s_pic18f4550_txif() == 0u) {
    }
    r2s_pic18f4550_txreg_write(buf[i]);
  }
}

uint16_t r2s_port_tx_free(void)
{
  /* The transmit register holds one byte. Reporting the real depth keeps
   * the core from queueing a frame the UART cannot take; 0xFFFF is the
   * escape for a port that blocks instead, and this one reports honestly. */
  return (r2s_pic18f4550_txif() != 0u) ? 1u : 0u;
}

uint32_t r2s_port_tick(void)
{
  uint16_t now = r2s_pic18f4550_tmr1_read();
  /* The subtraction is done in 16 bits so a wrap reads as the counts
   * since the last look instead of a huge jump backwards. */
  uint16_t delta = (uint16_t)(now - tick.last);

  tick.last = now;
  if (tick.per_tick == 0u) {
    return tick.count;
  }
  tick.elapsed += delta;
  while (tick.elapsed >= tick.per_tick) {
    tick.elapsed -= tick.per_tick;
    tick.count++;
  }
  return tick.count;
}
