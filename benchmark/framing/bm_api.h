// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* bm_api: what every benchmark framer provides. Each framer takes whole
 * frames in and out, so one host driver and one AVR firmware exercise all
 * of them the same way. Alternatives live here, never in core/src. */
#ifndef BM_API_H
#define BM_API_H

#include <stdint.h>

/* Largest decoded frame fed in: topic, seq, payload and check bytes. */
#define BM_DEC_CAP 128u
/* Largest wire image: SLIP doubles every byte, then the delimiter. */
#define BM_WIRE_CAP 260u

#endif
