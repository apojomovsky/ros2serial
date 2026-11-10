// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* bm_cobs: standard COBS byte stuffing for the benchmark. Codes open the
 * block they describe, the mirror image of the shipped rCOBS. */
#ifndef BM_COBS_H
#define BM_COBS_H

#include <stdint.h>

/* Encodes dec into wire. Returns the wire bytes, or 0 when wire_cap is
 * short. The caller sizes wire after bm_api.h, so 0 never fires there. */
uint16_t bm_cobs_encode(const uint8_t *dec, uint16_t dec_len, uint8_t *wire, uint16_t wire_cap);

/* Decodes len bytes in place: buf holds the encoding without its delimiter,
 * and the decoded frame overwrites it from the start. Writes the frame start
 * to dec and its length to dec_len. Returns 0, or -1 on a bad encoding. */
int bm_cobs_decode(uint8_t *buf, uint16_t len, const uint8_t **dec, uint16_t *dec_len);

#endif
