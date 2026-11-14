// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* PIC18F4550 registers: the EUSART and Timer1 the port drives, behind one
 * accessor per register the glue touches. The firmware build reaches the
 * real SFRs; the host test reaches fakes that model the two flags the glue
 * polls, so a test can tell a waiting byte from an empty receiver. */
#ifndef R2S_PIC18F4550_REGS_H
#define R2S_PIC18F4550_REGS_H

#include <stdint.h>

/* RCSTA: SPEN (bit 7) enables the serial port, CREN (bit 4) the receiver,
 * OERR (bit 1) reports a hardware overrun the glue clears by cycling
 * CREN. */
#define R2S_PIC18F4550_RCSTA_SPEN (1u << 7u)
#define R2S_PIC18F4550_RCSTA_CREN (1u << 4u)
#define R2S_PIC18F4550_RCSTA_OERR (1u << 1u)

/* TXSTA: TXEN (bit 5) starts the transmitter, BRGH (bit 2) picks the
 * high-speed divisor. */
#define R2S_PIC18F4550_TXSTA_TXEN (1u << 5u)
#define R2S_PIC18F4550_TXSTA_BRGH (1u << 2u)

/* TRISC: RC7 listens and RC6 drives, so the UART owns both pins. */
#define R2S_PIC18F4550_TRISC_RX (1u << 7u)
#define R2S_PIC18F4550_TRISC_TX (1u << 6u)

/* T1CON: TMR1ON (bit 0) runs the timer, T1RUN (bits 3:2) pick the
 * prescaler, 00 = 1:1, 01 = 1:2, 10 = 1:4, 11 = 1:8. The port runs at
 * 1:1 so a microsecond tick stays resolvable at 20 MHz. */
#define R2S_PIC18F4550_T1CON_TMR1ON (1u << 0u)
#define R2S_PIC18F4550_T1CON_T1RUN_MASK (3u << 2u)
#define R2S_PIC18F4550_T1CON_T1_1_1 (0u << 2u)

#ifdef R2S_PIC18F4550_HOST_TEST

typedef struct
{
  volatile uint8_t RCSTA;
  volatile uint8_t TXSTA;
  volatile uint8_t SPBRG;
  volatile uint8_t TRISC;
  volatile uint8_t T1CON;
  volatile uint16_t TMR1;
  /* The receive side: bytes the test queues, and how many it has taken.
   * A take clears the "byte waits" flag the way the register does. */
  uint8_t rx[8];
  uint16_t rx_count;
  uint16_t rx_taken;
  /* The transmit side: what the glue handed over, and whether the
   * register is still busy with the previous byte. */
  uint8_t tx[8];
  uint16_t tx_count;
  uint8_t tx_busy;
  /* Set when the glue cycles CREN to clear an overrun, so a test can see
   * the recovery instead of only its effect. */
  uint8_t cren_cycles;
} r2s_pic18f4550_regs_t;

extern r2s_pic18f4550_regs_t R2S_PIC18F4550_TEST_regs;

/// Clears the fake registers between checks.
void r2s_pic18f4550_test_regs_reset(void);

/// Queues bytes the receiver reports as waiting.
/**
 * \param[in] bytes the bytes to queue, in arrival order.
 * \param[in] count how many bytes bytes holds, at most 8.
 */
void r2s_pic18f4550_test_rx_queue(const uint8_t *bytes, uint16_t count);

static inline uint8_t r2s_pic18f4550_rcsta_read(void)
{
  return R2S_PIC18F4550_TEST_regs.RCSTA;
}

static inline void r2s_pic18f4550_rcsta_write(uint8_t value)
{
  /* A write that clears CREN and a later one that sets it is the overrun
   * recovery; count the pair so a test can assert it happened. */
  if ((R2S_PIC18F4550_TEST_regs.RCSTA & R2S_PIC18F4550_RCSTA_CREN) != 0u &&
    (value & R2S_PIC18F4550_RCSTA_CREN) == 0u)
  {
    R2S_PIC18F4550_TEST_regs.cren_cycles++;
  }
  R2S_PIC18F4550_TEST_regs.RCSTA = value;
}

static inline uint8_t r2s_pic18f4550_txsta_read(void)
{
  return R2S_PIC18F4550_TEST_regs.TXSTA;
}

static inline void r2s_pic18f4550_txsta_write(uint8_t value)
{
  R2S_PIC18F4550_TEST_regs.TXSTA = value;
}

static inline uint8_t r2s_pic18f4550_spbrg_read(void)
{
  return R2S_PIC18F4550_TEST_regs.SPBRG;
}

static inline void r2s_pic18f4550_spbrg_write(uint8_t value)
{
  R2S_PIC18F4550_TEST_regs.SPBRG = value;
}

static inline uint8_t r2s_pic18f4550_trisc_read(void)
{
  return R2S_PIC18F4550_TEST_regs.TRISC;
}

static inline void r2s_pic18f4550_trisc_write(uint8_t value)
{
  R2S_PIC18F4550_TEST_regs.TRISC = value;
}

static inline uint8_t r2s_pic18f4550_t1con_read(void)
{
  return R2S_PIC18F4550_TEST_regs.T1CON;
}

static inline void r2s_pic18f4550_t1con_write(uint8_t value)
{
  R2S_PIC18F4550_TEST_regs.T1CON = value;
}

static inline uint16_t r2s_pic18f4550_tmr1_read(void)
{
  return R2S_PIC18F4550_TEST_regs.TMR1;
}

static inline void r2s_pic18f4550_tmr1_write(uint16_t value)
{
  R2S_PIC18F4550_TEST_regs.TMR1 = value;
}

/* The receiver has a byte when the test queued one the glue has not taken. */
static inline uint8_t r2s_pic18f4550_rcif(void)
{
  return (uint8_t)(R2S_PIC18F4550_TEST_regs.rx_taken <
         R2S_PIC18F4550_TEST_regs.rx_count);
}

static inline uint8_t r2s_pic18f4550_rcreg_read(void)
{
  uint8_t byte = R2S_PIC18F4550_TEST_regs.rx[R2S_PIC18F4550_TEST_regs.rx_taken];

  R2S_PIC18F4550_TEST_regs.rx_taken++;
  return byte;
}

/* The transmit register is free unless the test holds it busy. */
static inline uint8_t r2s_pic18f4550_txif(void)
{
  return (uint8_t)(R2S_PIC18F4550_TEST_regs.tx_busy == 0u);
}

static inline void r2s_pic18f4550_txreg_write(uint8_t value)
{
  R2S_PIC18F4550_TEST_regs.tx[R2S_PIC18F4550_TEST_regs.tx_count] = value;
  R2S_PIC18F4550_TEST_regs.tx_count++;
}

#else

/* The device header epic-cc ships names every SFR the glue touches
 * (DS39632E, Table 5-1). RCIF is PIR1 bit 5 and TXIF bit 4. */
#include <pic18f4550.h>

#define R2S_PIC18F4550_PIR1_RCIF (1u << 5u)
#define R2S_PIC18F4550_PIR1_TXIF (1u << 4u)

static inline uint8_t r2s_pic18f4550_rcsta_read(void)
{
  return RCSTA;
}

static inline void r2s_pic18f4550_rcsta_write(uint8_t value)
{
  RCSTA = value;
}

static inline uint8_t r2s_pic18f4550_txsta_read(void)
{
  return TXSTA;
}

static inline void r2s_pic18f4550_txsta_write(uint8_t value)
{
  TXSTA = value;
}

static inline uint8_t r2s_pic18f4550_spbrg_read(void)
{
  return SPBRG;
}

static inline void r2s_pic18f4550_spbrg_write(uint8_t value)
{
  SPBRG = value;
}

static inline uint8_t r2s_pic18f4550_trisc_read(void)
{
  return TRISC;
}

static inline void r2s_pic18f4550_trisc_write(uint8_t value)
{
  TRISC = value;
}

static inline uint8_t r2s_pic18f4550_t1con_read(void)
{
  return T1CON;
}

static inline void r2s_pic18f4550_t1con_write(uint8_t value)
{
  T1CON = value;
}

static inline uint16_t r2s_pic18f4550_tmr1_read(void)
{
  return TMR1;
}

static inline void r2s_pic18f4550_tmr1_write(uint16_t value)
{
  TMR1 = value;
}

static inline uint8_t r2s_pic18f4550_rcif(void)
{
  return (uint8_t)((PIR1 & R2S_PIC18F4550_PIR1_RCIF) != 0u);
}

static inline uint8_t r2s_pic18f4550_rcreg_read(void)
{
  return RCREG;
}

static inline uint8_t r2s_pic18f4550_txif(void)
{
  return (uint8_t)((PIR1 & R2S_PIC18F4550_PIR1_TXIF) != 0u);
}

static inline void r2s_pic18f4550_txreg_write(uint8_t value)
{
  TXREG = value;
}

#endif /* R2S_PIC18F4550_HOST_TEST */

#endif /* R2S_PIC18F4550_REGS_H */
