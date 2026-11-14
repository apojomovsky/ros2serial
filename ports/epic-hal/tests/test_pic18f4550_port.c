// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* The PIC18F4550 glue's host test: the port contract against fake SFRs.
 * Fails unless the glue computes the baud divisor, accumulates the 16-bit
 * timer into the millisecond tick, enables both directions on init, and
 * moves bytes both ways through the faked EUSART. */
#include <stdint.h>
#include <stdio.h>

#include "r2s/r2s_port.h"
#include "r2s_pic18f4550_port.h"
#include "r2s_pic18f4550_regs.h"

/* The counts one tick spans at the rate this build compiles for, so the
 * checks hold at the default 1 kHz and at the simulator's 1 MHz. */
#define TICK_COUNTS (20000000u / (4u * R2S_TICK_HZ))

#define CHECK(cond) \
  do { \
    if (!(cond)) { \
      printf("fail line %d: %s\n", __LINE__, #cond); \
      return 1; \
    } \
  } while (0)

static int check_spbrg_truncates(void)
{
  /* 20 MHz at 115200 with BRGH=1 and BRG16=0 divides by 16: 10.85
   * truncates to a divisor of 10, and the register holds one less. 48 MHz
   * gives 26.04, so the register holds 25. */
  CHECK(r2s_pic18f4550_spbrg(20000000u, 115200u) == 9u);
  CHECK(r2s_pic18f4550_spbrg(48000000u, 115200u) == 25u);
  return 0;
}

static int check_spbrg_refuses_impossible(void)
{
  /* A zero baud would divide by zero, and a divisor past the 8-bit BRG
   * cannot be programmed, so the helper reports 0 for both. */
  CHECK(r2s_pic18f4550_spbrg(20000000u, 0u) == 0u);
  CHECK(r2s_pic18f4550_spbrg(1000000u, 115200u) == 0u);
  return 0;
}

static int check_cycles_per_tick_follows_the_tick_rate(void)
{
  /* The timer counts Fosc/4 at 1:1, so 20 MHz leaves 5000 counts per
   * tick at the default 1 kHz and 5 at the simulator's 1 MHz. */
  CHECK(r2s_pic18f4550_cycles_per_tick(20000000u) == TICK_COUNTS);
  CHECK(r2s_pic18f4550_cycles_per_tick(48000000u) == (uint16_t)(48000000u / (4u * R2S_TICK_HZ)));
  /* A zero clock would divide by zero, so the helper reports 0 instead. */
  CHECK(r2s_pic18f4550_cycles_per_tick(0u) == 0u);
  return 0;
}

static int check_init_enables_both_directions(void)
{
  r2s_pic18f4550_test_regs_reset();
  r2s_pic18f4550_init(20000000u, 115200u);
  /* SPEN and CREN on, so the receiver runs; TXEN on, so the transmitter
   * runs; the divisor from the same clock. */
  CHECK((R2S_PIC18F4550_TEST_regs.RCSTA & R2S_PIC18F4550_RCSTA_SPEN) != 0u);
  CHECK((R2S_PIC18F4550_TEST_regs.RCSTA & R2S_PIC18F4550_RCSTA_CREN) != 0u);
  CHECK((R2S_PIC18F4550_TEST_regs.TXSTA & R2S_PIC18F4550_TXSTA_TXEN) != 0u);
  CHECK(R2S_PIC18F4550_TEST_regs.SPBRG == 9u);
  /* RC6 drives the line out and RC7 listens, so the UART owns both pins. */
  CHECK((R2S_PIC18F4550_TEST_regs.TRISC & R2S_PIC18F4550_TRISC_RX) != 0u);
  CHECK((R2S_PIC18F4550_TEST_regs.TRISC & R2S_PIC18F4550_TRISC_TX) == 0u);
  /* Timer1 runs, 16-bit, with the 1:8 prescaler the count conversion
   * assumes, and it starts from zero so the tick begins at 0. */
  CHECK((R2S_PIC18F4550_TEST_regs.T1CON & R2S_PIC18F4550_T1CON_TMR1ON) != 0u);
  CHECK((R2S_PIC18F4550_TEST_regs.T1CON & R2S_PIC18F4550_T1CON_T1RUN_MASK) ==
    R2S_PIC18F4550_T1CON_T1_1_1);
  CHECK(R2S_PIC18F4550_TEST_regs.TMR1 == 0u);
  return 0;
}

static int check_read_takes_waiting_bytes(void)
{
  static const uint8_t queued[2] = {0x11u, 0x22u};
  uint8_t buf[4] = {0};

  r2s_pic18f4550_test_regs_reset();
  r2s_pic18f4550_init(20000000u, 115200u);
  r2s_pic18f4550_test_rx_queue(queued, 2u);
  /* The port asks for at most what the caller holds and copies in order. */
  CHECK(r2s_port_read(buf, 4u) == 2u);
  CHECK(buf[0] == 0x11u);
  CHECK(buf[1] == 0x22u);
  /* Nothing left: the next call reports none waiting, not an error. */
  CHECK(r2s_port_read(buf, 4u) == 0u);
  return 0;
}

static int check_read_respects_max(void)
{
  static const uint8_t queued[2] = {0x33u, 0x44u};
  uint8_t buf[1] = {0};

  r2s_pic18f4550_test_regs_reset();
  r2s_pic18f4550_init(20000000u, 115200u);
  r2s_pic18f4550_test_rx_queue(queued, 2u);
  /* One slot in the caller's buffer takes one byte and leaves the other. */
  CHECK(r2s_port_read(buf, 1u) == 1u);
  CHECK(buf[0] == 0x33u);
  CHECK(r2s_port_read(buf, 1u) == 1u);
  CHECK(buf[0] == 0x44u);
  return 0;
}

static int check_read_clears_an_overrun(void)
{
  static const uint8_t queued[1] = {0x55u};
  uint8_t buf[1] = {0};

  r2s_pic18f4550_test_regs_reset();
  r2s_pic18f4550_init(20000000u, 115200u);
  r2s_pic18f4550_test_rx_queue(queued, 1u);
  /* A latched overrun leaves the receiver deaf, so the glue cycles CREN
   * before it takes the byte. */
  R2S_PIC18F4550_TEST_regs.RCSTA |= (uint8_t)R2S_PIC18F4550_RCSTA_OERR;
  CHECK(r2s_port_read(buf, 1u) == 1u);
  CHECK(buf[0] == 0x55u);
  CHECK(R2S_PIC18F4550_TEST_regs.cren_cycles == 1u);
  /* The receiver runs again, which is the point of the cycle. */
  CHECK((R2S_PIC18F4550_TEST_regs.RCSTA & R2S_PIC18F4550_RCSTA_CREN) != 0u);
  return 0;
}

static int check_write_queues_in_order(void)
{
  static const uint8_t tx[3] = {0xA1u, 0xA2u, 0xA3u};

  r2s_pic18f4550_test_regs_reset();
  r2s_pic18f4550_init(20000000u, 115200u);
  r2s_port_write(tx, 3u);
  /* The port waits for the transmit register each byte, then hands it
   * over in the caller's order. */
  CHECK(R2S_PIC18F4550_TEST_regs.tx_count == 3u);
  CHECK(R2S_PIC18F4550_TEST_regs.tx[0] == 0xA1u);
  CHECK(R2S_PIC18F4550_TEST_regs.tx[1] == 0xA2u);
  CHECK(R2S_PIC18F4550_TEST_regs.tx[2] == 0xA3u);
  return 0;
}

static int check_tx_free_reports_the_register(void)
{
  r2s_pic18f4550_test_regs_reset();
  r2s_pic18f4550_init(20000000u, 115200u);
  /* One byte fits the transmit register, so the core may write one
   * byte now. */
  R2S_PIC18F4550_TEST_regs.tx_busy = 0u;
  CHECK(r2s_port_tx_free() == 1u);
  R2S_PIC18F4550_TEST_regs.tx_busy = 1u;
  CHECK(r2s_port_tx_free() == 0u);
  return 0;
}

static int check_tick_converts_the_count(void)
{
  r2s_pic18f4550_test_regs_reset();
  r2s_pic18f4550_init(20000000u, 115200u);
  /* One tick's worth of counts is one tick, whatever the compiled rate. */
  R2S_PIC18F4550_TEST_regs.TMR1 = TICK_COUNTS;
  CHECK(r2s_port_tick() == 1u);
  R2S_PIC18F4550_TEST_regs.TMR1 = (uint16_t)(TICK_COUNTS * 3u);
  CHECK(r2s_port_tick() == 3u);
  return 0;
}

static int check_tick_accumulates_the_hardware_wrap(void)
{
  uint32_t before;
  uint32_t after;
  uint16_t half;

  r2s_pic18f4550_test_regs_reset();
  r2s_pic18f4550_init(20000000u, 115200u);
  /* Step to just under the 16-bit top and note the tick, then cross it.
   * Two half-ticks across the wrap are one tick, so the count must move
   * forward by exactly one instead of restarting when the hardware does. */
  half = (uint16_t)(TICK_COUNTS / 2u);
  R2S_PIC18F4550_TEST_regs.TMR1 = (uint16_t)(0xFFFFu - half);
  before = r2s_port_tick();
  R2S_PIC18F4550_TEST_regs.TMR1 = half;
  after = r2s_port_tick();
  /* One whole tick went by, measured across the wrap. */
  CHECK((uint32_t)(after - before) == 1u);
  return 0;
}

static int check_tick_keeps_counting_past_a_second(void)
{
  uint32_t i;

  r2s_pic18f4550_test_regs_reset();
  r2s_pic18f4550_init(20000000u, 115200u);
  /* Sixty passes of the Timer1 range is far more counts than a 16-bit
   * count holds, so a tick that lost them would read low. Both compiled
   * rates clear this bound by a wide margin, and a port that discarded a
   * delta above 0x8000 as a glitch would fail it. */
  R2S_PIC18F4550_TEST_regs.TMR1 = 0u;
  (void)r2s_port_tick();
  for (i = 0u; i < 60u; i++) {
    R2S_PIC18F4550_TEST_regs.TMR1 = 60000u;
    (void)r2s_port_tick();
    R2S_PIC18F4550_TEST_regs.TMR1 = 0u;
    (void)r2s_port_tick();
  }
  CHECK(r2s_port_tick() > 700u);
  return 0;
}

int main(void)
{
  int rc = 0;

  rc |= check_spbrg_truncates();
  rc |= check_spbrg_refuses_impossible();
  rc |= check_cycles_per_tick_follows_the_tick_rate();
  rc |= check_init_enables_both_directions();
  rc |= check_read_takes_waiting_bytes();
  rc |= check_read_respects_max();
  rc |= check_read_clears_an_overrun();
  rc |= check_write_queues_in_order();
  rc |= check_tx_free_reports_the_register();
  rc |= check_tick_converts_the_count();
  rc |= check_tick_accumulates_the_hardware_wrap();
  rc |= check_tick_keeps_counting_past_a_second();
  return rc;
}
