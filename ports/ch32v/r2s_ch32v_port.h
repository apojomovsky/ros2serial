// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* CH32V003 port selection: USART1 at 115200 8-N-1 plus the SysTick tick.
 * main() calls r2s_ch32v_init() once at boot; the core reaches the
 * hardware through the port contract after that. */
#ifndef R2S_CH32V_PORT_H
#define R2S_CH32V_PORT_H

#include <stdint.h>

#include "r2s_ch32v_regs.h"

/// Switches the part to 48 MHz and starts USART1 plus the tick.
/// /**
///  * Moves the system clock from the 24 MHz reset HSI to 48 MHz through
///  * the fixed x2 PLL, enables GPIOD and USART1, maps TX to PD5 and RX
///  * to PD6, and starts the SysTick counting free at HCLK/8. The USART
///  * runs 8-N-1 with transmitter and receiver on.
///  *
///  * \param[in] hclk_hz the clock init leaves running, for the divisor.
///  * \param[in] baud the USART baud rate, for example 115200.
///  * \pre hclk_hz matches the switched clock and baud is nonzero.
///  */
void r2s_ch32v_init(uint32_t hclk_hz, uint32_t baud);

#endif
