/* bm_cobs: standard COBS byte stuffing for the benchmark. Codes open the
 * block they describe; the decoder walks front to back, in place. */
#include "bm_cobs.h"

#include <stdint.h>

uint16_t bm_cobs_encode(const uint8_t *dec, uint16_t dec_len, uint8_t *wire, uint16_t wire_cap)
{
  uint32_t need = (uint32_t)dec_len + (uint32_t)dec_len / 254u + 1u;
  uint16_t out = 1u;
  uint16_t code_at = 0u;
  uint8_t code = 1u;
  uint16_t i;
  if ((uint32_t)wire_cap < need) {
    return 0u;
  }
  wire[0] = 0u;
  for (i = 0u; i < dec_len; i++) {
    if (dec[i] == 0x00u) {
      wire[code_at] = code;
      code_at = out;
      wire[out] = 0u;
      out++;
      code = 1u;
    } else {
      wire[out] = dec[i];
      out++;
      code++;
      if (code == 0xFFu) {
        wire[code_at] = code;
        code_at = out;
        wire[out] = 0u;
        out++;
        code = 1u;
      }
    }
  }
  wire[code_at] = code;
  return out;
}

int bm_cobs_decode(uint8_t *buf, uint16_t len, const uint8_t **dec, uint16_t *dec_len)
{
  uint16_t read = 0u;
  uint16_t write = 0u;
  uint16_t i;
  for (i = 0u; i < len; i++) {
    if (buf[i] == 0x00u) {
      return -1;
    }
  }
  while (read < len) {
    uint8_t code = buf[read];
    uint8_t k;
    if (code == 0u || (uint32_t)read + (uint32_t)code > (uint32_t)len) {
      return -1;
    }
    for (k = 1u; k < code; k++) {
      buf[write] = buf[read + k];
      write++;
    }
    read = (uint16_t)(read + code);
    if (code != 0xFFu && read < len) {
      buf[write] = 0x00u;
      write++;
    }
  }
  *dec = buf;
  *dec_len = write;
  return 0;
}
