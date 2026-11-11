// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Template registers: one UART plus a free-running counter. Copy this
 * file with the port, rename TEMPLATE to the new port, and point the
 * two base addresses at the new device. */
#ifndef R2S_TEMPLATE_REGS_H
#define R2S_TEMPLATE_REGS_H

#include <stdint.h>

typedef struct
{
  volatile uint32_t STAT;
  volatile uint32_t DATA;
  volatile uint32_t BRR;
  volatile uint32_t CTL;
} r2s_template_uart_t;

typedef struct
{
  volatile uint32_t CNT;
} r2s_template_timer_t;

/* The status bits the port polls: space to send, a byte to read. */
#define R2S_TEMPLATE_STAT_TXE (1u << 7u)
#define R2S_TEMPLATE_STAT_RXNE (1u << 5u)

/* The control bits the init sets: the UART on, both directions on. */
#define R2S_TEMPLATE_CTL_UE (1u << 0u)
#define R2S_TEMPLATE_CTL_TE (1u << 1u)
#define R2S_TEMPLATE_CTL_RE (1u << 2u)

/* The timer runs free once STE is set; the tick reads CNT with no interrupt. */
#define R2S_TEMPLATE_TIMER_STE (1u << 0u)

#ifdef R2S_TEMPLATE_HOST_TEST
extern r2s_template_uart_t R2S_TEMPLATE_TEST_uart;
extern r2s_template_timer_t R2S_TEMPLATE_TEST_timer;
extern uint8_t R2S_TEMPLATE_TEST_tx[256];
extern uint16_t R2S_TEMPLATE_TEST_tx_count;

#define R2S_TEMPLATE_UART (&R2S_TEMPLATE_TEST_uart)
#define R2S_TEMPLATE_TIMER (&R2S_TEMPLATE_TEST_timer)
#else
#define R2S_TEMPLATE_UART ((r2s_template_uart_t *)0x40013800u)
#define R2S_TEMPLATE_TIMER ((r2s_template_timer_t *)0xE000E010u)
#endif

/* Zeroes the fakes, so each test starts from reset values. */
void r2s_template_test_regs_reset(void);

/// Divisor for a baud rate from the given clock.
/**
 * Truncates, so the UART runs at most the asked rate. A zero baud would
 * divide by zero, so the helper reports no divisor instead.
 *
 * \param[in] clk_hz the clock the init leaves running.
 * \param[in] baud the asked baud rate, for example 115200.
 * \return The divisor, or 0 when baud is 0.
 */
static inline uint16_t r2s_template_brr(uint32_t clk_hz, uint32_t baud)
{
  if (baud == 0u) {
    return 0u;
  }
  return (uint16_t)(clk_hz / baud);
}

/// Milliseconds from a free-running counter.
/**
 * Wraps with the counter, the same way the tick the core reads does.
 * A zero rate would divide by zero, so the helper reports 0 instead.
 *
 * \param[in] cnt the current counter value.
 * \param[in] counts_per_ms how many counts make a millisecond.
 * \return The whole milliseconds, or 0 when counts_per_ms is 0.
 */
static inline uint32_t r2s_template_ms(uint32_t cnt, uint32_t counts_per_ms)
{
  if (counts_per_ms == 0u) {
    return 0u;
  }
  return cnt / counts_per_ms;
}

/* Sends one byte: the DATA store on target, the capture buffer on host.
 * The host form lets the contract test read back the order bytes left in. */
#ifdef R2S_TEMPLATE_HOST_TEST
static inline void r2s_template_uart_send(r2s_template_uart_t *uart, uint8_t byte)
{
  uart->DATA = (uint32_t)byte;
  if (R2S_TEMPLATE_TEST_tx_count < sizeof(R2S_TEMPLATE_TEST_tx)) {
    R2S_TEMPLATE_TEST_tx[R2S_TEMPLATE_TEST_tx_count] = byte;
    R2S_TEMPLATE_TEST_tx_count++;
  }
}
#else
static inline void r2s_template_uart_send(r2s_template_uart_t *uart, uint8_t byte)
{
  uart->DATA = (uint32_t)byte;
}
#endif

/* Reads one byte: the DATA load on both builds, which the test primes.
 * The host form also clears RXNE, the way reading DATA does in hardware,
 * so a primed byte reads once and the port then reports empty. */
static inline uint8_t r2s_template_uart_recv(r2s_template_uart_t *uart)
{
  uint8_t byte = (uint8_t)uart->DATA;
#ifdef R2S_TEMPLATE_HOST_TEST
  uart->STAT &= (uint32_t)(~R2S_TEMPLATE_STAT_RXNE);
#endif
  return byte;
}

#endif
