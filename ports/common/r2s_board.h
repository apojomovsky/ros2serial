// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* board: clocks plus UART setup behind one call, so the shared firmware
 * stays target-free. Each port glue file implements it. */
#ifndef R2S_BOARD_H
#define R2S_BOARD_H

/// Starts clocks, the UART at 115200 8-N-1, and the millisecond tick.
/**
 * Runs once from main before the session configures: the UART the port
 * functions use is idle until this returns.
 */
void r2s_board_init(void);

#endif
