// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Template port selection: the UART plus the tick the core reads through
 * the port contract. main() calls r2s_template_init() once at boot. */
#ifndef R2S_TEMPLATE_PORT_H
#define R2S_TEMPLATE_PORT_H

#include <stdint.h>

/// Starts the UART and the tick source.
/**
 * Sets the baud divisor from clk_hz, starts the UART at 8-N-1 with
 * transmitter and receiver on, and starts the free-running counter the
 * tick reads. Polling only, so no interrupt setup is involved.
 *
 * \param[in] clk_hz the clock the init leaves running, for the divisor.
 * \param[in] baud the UART baud rate, for example 115200.
 * \pre clk_hz is nonzero and baud is nonzero.
 */
void r2s_template_init(uint32_t clk_hz, uint32_t baud);

#endif
