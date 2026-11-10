// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* bm_rs: a rosserial-style length-prefixed framing for the benchmark. It
 * follows rosserial's shape (sync byte, version, length with an additive
 * check, 16-bit topic, message checksum) without copying its code. */
#ifndef BM_RS_H
#define BM_RS_H

#include <stdint.h>

/* The bytes on the wire. The version byte is required, not skipped. */
#define BM_RS_SYNC 0xFFu
#define BM_RS_VERSION 0x01u
/* The longest body scanned: one wild length byte cannot claim the line. */
#define BM_RS_BODY_MAX 256u

/* Builds one frame from topic, seq and payload. Returns the wire bytes, or
 * 0 when wire_cap is short. */
uint16_t bm_rs_encode(
  uint16_t topic, uint8_t seq, const uint8_t *payload, uint16_t payload_len,
  uint8_t *wire, uint16_t wire_cap);

/* Finds the first frame at or after stream. On success returns 1 and sets
 * topic, seq, payload, payload_len and consumed, the frame's end offset
 * from stream. Returns 0 when no frame starts in the stream. */
int bm_rs_find(
  const uint8_t *stream, uint16_t len, uint16_t *topic, uint8_t *seq, const uint8_t **payload,
  uint16_t *payload_len, uint16_t *consumed);

#endif
