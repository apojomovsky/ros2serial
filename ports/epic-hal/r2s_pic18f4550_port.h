// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* PIC18F4550 port: the core's port contract over EUSART1 with Timer1 as
 * the millisecond source. Polling only, so the glue needs no interrupt
 * vector and the simulator can drive it one byte at a time. */
#ifndef R2S_PIC18F4550_PORT_H
#define R2S_PIC18F4550_PORT_H

#include <stdint.h>

/// Starts the EUSART and Timer1.
/**
 * Programs SPBRG from clk_hz, starts the EUSART at 8-N-1 with the
 * transmitter and receiver on, takes RC6 and RC7 for the UART, and starts
 * Timer1 at 1:1 from zero so the tick begins at 0.
 *
 * \param[in] clk_hz the instruction clock's source, in Hz.
 * \param[in] baud the UART baud rate, for example 115200.
 * \pre clk_hz and baud are nonzero.
 */
void r2s_pic18f4550_init(uint32_t clk_hz, uint32_t baud);

/// Computes the SPBRG value for a baud rate with BRGH high and BRG16 low.
/**
 * The divisor is clk_hz / (16 * baud), floored: the high-baud-rate mode
 * halves the 64-clock divisor of the power-on setting, which is what
 * makes 115200 reachable from a 20 MHz part.
 *
 * \param[in] clk_hz the oscillator frequency, in Hz.
 * \param[in] baud the wanted baud rate.
 * \return The divisor, or 0 when baud is 0 or the divisor does not fit
 *   the 8-bit SPBRG.
 */
uint8_t r2s_pic18f4550_spbrg(uint32_t clk_hz, uint32_t baud);

/// Counts Timer1 ticks in one tick of the core's clock.
/**
 * Timer1 counts the instruction clock (clk_hz / 4) at 1:1, so one tick of
 * R2S_TICK_HZ is clk_hz / (4 * R2S_TICK_HZ) counts. The result is what
 * the port accumulates, so the tick the core reads advances at the rate
 * the HELLO announces rather than at a rate of the port's own choosing.
 *
 * \param[in] clk_hz the oscillator frequency, in Hz.
 * \return The counts per tick, or 0 when clk_hz is 0 or one tick is
 *   shorter than a single count.
 */
uint16_t r2s_pic18f4550_cycles_per_tick(uint32_t clk_hz);

#endif /* R2S_PIC18F4550_PORT_H */
