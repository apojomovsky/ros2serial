/* bm_slip: SLIP after RFC 1055 for the benchmark. The encoding quotes END
 * and ESC and appends its own END; decoding splits on END outside. */
#include "bm_slip.h"

#include <stdint.h>

uint16_t bm_slip_encode(const uint8_t *dec, uint16_t dec_len, uint8_t *wire, uint16_t wire_cap)
{
  uint32_t need = (uint32_t)dec_len * 2u + 1u;
  uint16_t out = 0u;
  uint16_t i;
  if ((uint32_t)wire_cap < need) {
    return 0u;
  }
  for (i = 0u; i < dec_len; i++) {
    if (dec[i] == BM_SLIP_END) {
      wire[out] = BM_SLIP_ESC;
      wire[out + 1u] = BM_SLIP_ESC_END;
      out = (uint16_t)(out + 2u);
    } else if (dec[i] == BM_SLIP_ESC) {
      wire[out] = BM_SLIP_ESC;
      wire[out + 1u] = BM_SLIP_ESC_ESC;
      out = (uint16_t)(out + 2u);
    } else {
      wire[out] = dec[i];
      out++;
    }
  }
  wire[out] = BM_SLIP_END;
  out++;
  return out;
}

int bm_slip_decode(uint8_t *buf, uint16_t len, const uint8_t **dec, uint16_t *dec_len)
{
  uint16_t read = 0u;
  uint16_t write = 0u;
  while (read < len) {
    uint8_t byte = buf[read];
    if (byte == BM_SLIP_END) {
      return -1;
    }
    if (byte == BM_SLIP_ESC) {
      uint8_t follower;
      if ((uint32_t)read + 1u >= (uint32_t)len) {
        return -1;
      }
      follower = buf[read + 1u];
      if (follower == BM_SLIP_ESC_END) {
        buf[write] = BM_SLIP_END;
      } else if (follower == BM_SLIP_ESC_ESC) {
        buf[write] = BM_SLIP_ESC;
      } else {
        return -1;
      }
      write++;
      read = (uint16_t)(read + 2u);
    } else {
      buf[write] = byte;
      write++;
      read++;
    }
  }
  *dec = buf;
  *dec_len = write;
  return 0;
}
