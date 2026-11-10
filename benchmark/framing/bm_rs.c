// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* bm_rs: a rosserial-style length-prefixed framing for the benchmark. The
 * scanner resynchronizes by hunting the next sync byte, the price of having
 * no delimiter. Lengths are capped so one wild byte cannot claim the line. */
#include "bm_rs.h"

#include <stdint.h>

uint16_t bm_rs_encode(
  uint16_t topic, uint8_t seq, const uint8_t *payload, uint16_t payload_len,
  uint8_t *wire, uint16_t wire_cap)
{
  uint32_t body_len = (uint32_t)payload_len + 4u;
  uint16_t len;
  uint16_t i;
  uint8_t check = 0u;
  if (body_len > (uint32_t)BM_RS_BODY_MAX) {
    return 0u;
  }
  len = (uint16_t)body_len;
  if ((uint32_t)wire_cap < (uint32_t)len + 5u) {
    return 0u;
  }
  wire[0] = BM_RS_SYNC;
  wire[1] = BM_RS_VERSION;
  wire[2] = (uint8_t)len;
  wire[3] = (uint8_t)(len >> 8);
  wire[4] = (uint8_t)(0u - wire[2] - wire[3]);
  wire[5] = (uint8_t)topic;
  wire[6] = (uint8_t)(topic >> 8);
  wire[7] = seq;
  for (i = 0u; i < payload_len; i++) {
    wire[8u + i] = payload[i];
  }
  for (i = 0u; i < (uint16_t)(len - 1u); i++) {
    check = (uint8_t)(check + wire[5u + i]);
  }
  wire[(uint16_t)(5u + len - 1u)] = (uint8_t)(0u - check);
  return (uint16_t)(len + 5u);
}

int bm_rs_find(
  const uint8_t *stream, uint16_t len, uint16_t *topic, uint8_t *seq, const uint8_t **payload,
  uint16_t *payload_len, uint16_t *consumed)
{
  uint32_t i = 0u;
  while (i + 5u <= (uint32_t)len) {
    uint16_t flen;
    uint32_t end;
    uint16_t k;
    uint8_t sum = 0u;
    if (stream[i] != BM_RS_SYNC || stream[i + 1u] != BM_RS_VERSION) {
      i++;
      continue;
    }
    flen = (uint16_t)(stream[i + 2u] | ((uint16_t)stream[i + 3u] << 8));
    if (flen < 4u || flen > BM_RS_BODY_MAX) {
      i++;
      continue;
    }
    if ((uint8_t)(stream[i + 2u] + stream[i + 3u] + stream[i + 4u]) != 0u) {
      i++;
      continue;
    }
    end = i + 5u + (uint32_t)flen;
    if (end > (uint32_t)len) {
      return 0;
    }
    for (k = 0u; k < flen; k++) {
      sum = (uint8_t)(sum + stream[i + 5u + k]);
    }
    if (sum != 0u) {
      i++;
      continue;
    }
    *topic = (uint16_t)(stream[i + 5u] | ((uint16_t)stream[i + 6u] << 8));
    *seq = stream[i + 7u];
    *payload = stream + i + 8u;
    *payload_len = (uint16_t)(flen - 4u);
    *consumed = (uint16_t)end;
    return 1;
  }
  return 0;
}
