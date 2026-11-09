/* Bench firmware: one framer encodes and decodes the benchmark payloads,
 * timing each side with Timer1, and reports over UART0. Buffers are static
 * like the shipped framer's, so the stack paint measures call depth only. */
#include <avr/io.h>
#include <stddef.h>
#include <stdint.h>
#include <util/delay.h>

#include "bm_api.h"
#include "bm_cobs.h"
#include "bm_crc_table.h"
#include "bm_rs.h"
#include "bm_slip.h"
#include "payloads.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_rcobs.h"

#define BAUD 115200UL
#define UBRR_VALUE ((F_CPU / 16UL / BAUD) - 1UL)

#ifndef BM_FRAMER
#define BM_FRAMER 0
#endif
#ifndef BM_CRC_TABLE
#define BM_CRC_TABLE 0
#endif

/* Payload table order: the report numbers them P1 to P6 in this order. */
static const uint8_t *const bm_inputs[6] = {bm_p1, bm_p2, bm_p3, bm_p4, bm_p5, bm_p6};
static const uint16_t bm_input_lens[6] = {
  sizeof bm_p1, sizeof bm_p2, sizeof bm_p3, sizeof bm_p4, sizeof bm_p5, sizeof bm_p6,
};

#if BM_FRAMER != 3
static uint8_t bench_dec[BM_DEC_CAP];
#endif
static uint8_t bench_wire[BM_WIRE_CAP];

static void uart_init(void)
{
  UBRR0 = (uint16_t)UBRR_VALUE;
  UCSR0B = (1u << TXEN0);
  UCSR0C = (1u << UCSZ01) | (1u << UCSZ00);
}

static void uart_byte(uint8_t byte)
{
  while ((UCSR0A & (1u << UDRE0)) == 0u) {
  }
  UDR0 = byte;
}

static void uart_str(const char *text)
{
  while (*text != 0) {
    uart_byte((uint8_t)*text);
    text++;
  }
}

static void uart_u16(uint16_t value)
{
  char digits[6];
  uint8_t count = 0u;
  if (value == 0u) {
    uart_byte('0');
    return;
  }
  while (value > 0u) {
    digits[count] = (char)('0' + value % 10u);
    count++;
    value = (uint16_t)(value / 10u);
  }
  while (count > 0u) {
    count--;
    uart_byte((uint8_t)digits[count]);
  }
}

#if BM_FRAMER != 3
static uint16_t crc_of(const uint8_t *data, uint16_t len)
{
#if BM_CRC_TABLE
  return bm_crc16_table_update_buf(R2S_CRC16_INIT, data, len);
#else
  return r2s_crc16_update_buf(R2S_CRC16_INIT, data, len);
#endif
}
#endif

/* Encodes one frame: topic, seq and payload go in, the wire image, with its
 * delimiter where the framer puts one, comes out. Returns its length. */
static uint16_t do_encode(
  uint16_t topic, uint8_t seq, const uint8_t *payload, uint16_t payload_len,
  uint8_t *wire)
{
#if BM_FRAMER == 3
  return bm_rs_encode(topic, seq, payload, payload_len, wire, BM_WIRE_CAP);
#else
  uint16_t raw_len = (uint16_t)(payload_len + 2u);
  uint16_t crc;
  uint16_t wire_len = 0u;
  bench_dec[0] = (uint8_t)topic;
  bench_dec[1] = seq;
  for (uint16_t i = 0u; i < payload_len; i++) {
    bench_dec[2u + i] = payload[i];
  }
  crc = crc_of(bench_dec, raw_len);
  bench_dec[raw_len] = (uint8_t)crc;
  bench_dec[raw_len + 1u] = (uint8_t)(crc >> 8);
#if BM_FRAMER == 0
  {
    r2s_rcobs_encoder_t enc;
    r2s_rcobs_encoder_init(&enc);
    for (uint16_t i = 0u; i < (uint16_t)(raw_len + 2u); i++) {
      wire_len += r2s_rcobs_encode_byte(&enc, bench_dec[i], wire + wire_len);
    }
    wire_len += r2s_rcobs_encode_end(&enc, wire + wire_len);
    wire[wire_len] = 0x00u;
    wire_len++;
  }
#elif BM_FRAMER == 1
  wire_len = bm_cobs_encode(bench_dec, (uint16_t)(raw_len + 2u), wire, BM_WIRE_CAP);
  wire[wire_len] = 0x00u;
  wire_len++;
#elif BM_FRAMER == 2
  wire_len = bm_slip_encode(bench_dec, (uint16_t)(raw_len + 2u), wire, BM_WIRE_CAP);
#endif
  return wire_len;
#endif
}

/* Decodes one wire image in place and checks it against the frame do_encode
 * built. Returns 1 when the bytes match and the check passes. */
static uint8_t do_decode(
  uint8_t *wire, uint16_t wire_len, uint16_t topic, uint8_t seq,
  const uint8_t *payload, uint16_t payload_len)
{
#if BM_FRAMER == 3
  uint16_t found_topic = 0u;
  uint8_t found_seq = 0u;
  const uint8_t *found_payload = NULL;
  uint16_t found_len = 0u;
  uint16_t consumed = 0u;
  uint16_t i;
  if (!bm_rs_find(wire, wire_len, &found_topic, &found_seq, &found_payload, &found_len,
    &consumed))
  {
    return 0u;
  }
  if (found_topic != topic || found_seq != seq || found_len != payload_len) {
    return 0u;
  }
  for (i = 0u; i < payload_len; i++) {
    if (found_payload[i] != payload[i]) {
      return 0u;
    }
  }
  return 1u;
#else
  const uint8_t *dec = NULL;
  uint16_t dec_len = 0u;
  int status = -1;
  uint16_t i;
#if BM_FRAMER == 0
  status = r2s_rcobs_decode(wire, wire_len - 1u, (uint8_t **)&dec, &dec_len);
#elif BM_FRAMER == 1
  status = bm_cobs_decode(wire, wire_len - 1u, &dec, &dec_len);
#elif BM_FRAMER == 2
  status = bm_slip_decode(wire, wire_len - 1u, &dec, &dec_len);
#endif
  if (status != 0) {
    return 0u;
  }
  if (dec_len != (uint16_t)(payload_len + 4u)) {
    return 0u;
  }
  if (dec[0] != (uint8_t)topic || dec[1] != seq) {
    return 0u;
  }
  for (i = 0u; i < payload_len; i++) {
    if (dec[2u + i] != payload[i]) {
      return 0u;
    }
  }
  return (uint8_t)(crc_of(dec, dec_len) == 0u);
#endif
}

static void report_once(void)
{
  uint8_t all_ok = 1u;
  uart_str("BM START framer=");
  uart_u16((uint16_t)BM_FRAMER);
  uart_str(" table=");
  uart_u16((uint16_t)BM_CRC_TABLE);
  uart_str("\r\n");
  for (uint8_t p = 0u; p < 6u; p++) {
    const uint8_t *payload = bm_inputs[p];
    uint16_t payload_len = bm_input_lens[p];
    uint16_t wire_len;
    uint16_t enc_cycles;
    uint16_t dec_cycles;
    uint8_t ok;
    uint8_t overflow;
    TIFR1 = (1u << TOV1);
    TCNT1 = 0u;
    wire_len = do_encode(0x10u, p, payload, payload_len, bench_wire);
    enc_cycles = TCNT1;
    overflow = (uint8_t)((TIFR1 & (1u << TOV1)) != 0u);
    TIFR1 = (1u << TOV1);
    TCNT1 = 0u;
    ok = do_decode(bench_wire, wire_len, 0x10u, p, payload, payload_len);
    dec_cycles = TCNT1;
    overflow |= (uint8_t)((TIFR1 & (1u << TOV1)) != 0u);
    if (!ok || overflow || wire_len == 0u) {
      all_ok = 0u;
    }
    uart_str("BM PAYLOAD ");
    uart_u16((uint16_t)(p + 1u));
    uart_str(" declen=");
    uart_u16((uint16_t)(payload_len + 4u));
    uart_str(" wirelen=");
    uart_u16(wire_len);
    uart_str(" enc=");
    uart_u16(enc_cycles);
    uart_str(" dec=");
    uart_u16(dec_cycles);
    uart_str(" ok=");
    uart_u16(ok);
    uart_str("\r\n");
  }
  uart_str("BM DONE ok=");
  uart_u16(all_ok);
  uart_str("\r\n");
}

int main(void)
{
  uart_init();
  TCCR1A = 0u;
  TCCR1B = (1u << CS10);
  for (uint8_t round = 0u; round < 3u; round++) {
    report_once();
    _delay_ms(100.0);
  }
  for (;; ) {
  }
}
