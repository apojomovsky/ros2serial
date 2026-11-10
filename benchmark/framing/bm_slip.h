// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* bm_slip: SLIP after RFC 1055 for the benchmark. END marks the frame,
 * ESC quotes END and itself. */
#ifndef BM_SLIP_H
#define BM_SLIP_H

#include <stdint.h>

/* The bytes on the wire. END delimits, ESC starts a two byte quote. */
#define BM_SLIP_END 0xC0u
#define BM_SLIP_ESC 0xDBu
#define BM_SLIP_ESC_END 0xDCu
#define BM_SLIP_ESC_ESC 0xDDu

/* Encodes dec into wire with a trailing END. Returns the wire bytes, or 0
 * when wire_cap is short. */
uint16_t bm_slip_encode(const uint8_t *dec, uint16_t dec_len, uint8_t *wire, uint16_t wire_cap);

/* Decodes one segment between END markers in place, like bm_cobs_decode.
 * Returns 0, or -1 on a bad escape or a stray END. */
int bm_slip_decode(uint8_t *buf, uint16_t len, const uint8_t **dec, uint16_t *dec_len);

#endif
