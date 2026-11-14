// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* AVR check firmware for the service table: a table placed with R2S_FLASH, as
 * the generator emits it, binds through r2s_services_configure() and answers
 * the walk with its strings, hashes and role. The check script runs it under
 * simavr; the firmware prints PASS or the number of the step that failed and
 * sleeps, which ends the run. A host build cannot show this: there the flash
 * macros pass plain memory through, and only the target separates the two
 * address spaces. */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <stdint.h>
#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_arch.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_services.h"
#include "r2s/r2s_session.h"

#define BAUD 115200UL
#define UBRR_VALUE (F_CPU / 16UL / BAUD - 1UL)

/* A served reset and a called trigger, as the generator lays them out. */
static const r2s_service_t R2S_FLASH check_services[] = {
  {"std_srvs/srv/Trigger", "reset", R2S_SVC_SERVER, {0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u,
      0x07u, 0x08u}},
  {"std_srvs/srv/Trigger", "trigger", R2S_SVC_CLIENT, {0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u,
      0x17u, 0x18u}},
};

/* A QUERY for index 0 and the DECLARE it must answer, both from the Python
 * reference codec, so the comparison pins the whole frame: opcode, index,
 * role, hash, both length varints and both names (SVR-1, SVR-2, SVR-3). */
static const uint8_t query[] = {0x07u, 0x02u, 0x01u, 0x02u, 0xd8u, 0x4du, 0x03u, 0x00u};
static const uint8_t declare[] = {0x07u, 0x02u, 0x02u, 0x02u, 0x01u, 0x01u, 0x02u, 0x03u, 0x04u,
  0x05u, 0x06u, 0x07u, 0x08u, 0x14u, 's', 't', 'd', '_', 's', 'r', 'v', 's', '/', 's', 'r', 'v',
  '/', 'T', 'r', 'i', 'g', 'g', 'e', 'r', 0x05u, 'r', 'e', 's', 'e', 't', 0xf6u, 0x9cu, 0x26u,
  0x00u};

static uint16_t scripted_at;
static uint8_t captured[sizeof declare];
static uint16_t captured_len;

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t n = 0u;

  while (n < max && scripted_at < (uint16_t)sizeof query) {
    buf[n] = query[scripted_at];
    n++;
    scripted_at++;
  }
  return n;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len && captured_len < (uint16_t)sizeof captured; i++) {
    captured[captured_len] = buf[i];
    captured_len++;
  }
}

uint16_t r2s_port_tx_free(void)
{
  return 0xFFFFu;
}

/* A fixed tick: only the solicited answer matters, so no timer fires. */
uint32_t r2s_port_tick(void)
{
  return 0u;
}

static void uart_put(char c)
{
  loop_until_bit_is_set(UCSR0A, UDRE0);
  UDR0 = (uint8_t)c;
}

static void uart_puts(r2s_fstr_t text)
{
  uint16_t i = 0u;
  char c;

  while ((c = (char)R2S_FLASH_READ_BYTE((const uint8_t *)text + i)) != '\0') {
    uart_put(c);
    i++;
  }
}

/* Prints the verdict, waits for the byte to leave, and halts: simavr ends the
 * run on the sleep. */
static void finish(uint8_t step)
{
  if (step == 0u) {
    uart_puts(R2S_FSTR("r2s-flash-services: PASS\n"));
  } else {
    uart_puts(R2S_FSTR("r2s-flash-services: FAIL step "));
    uart_put((char)('0' + step));
    uart_put('\n');
  }
  loop_until_bit_is_set(UCSR0A, UDRE0);
  cli();
  sleep_cpu();
  for (;; ) {
  }
}

int main(void)
{
  r2s_service_t entry;

  UBRR0 = (uint16_t)UBRR_VALUE;
  UCSR0B = (1 << RXEN0) | (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);

  if (r2s_session_configure("flashsvc", NULL, 0u) != R2S_OK) {
    finish(1u);
  }
  /* The bind reads every entry through the flash reader: before that, an
   * R2S_FLASH table read as RAM refused here on AVR. */
  if (r2s_services_configure(check_services, 2u) != R2S_OK) {
    finish(2u);
  }
  if (r2s_services_count() != 2u) {
    finish(3u);
  }
  if (r2s_services_role(0u) != R2S_SVC_SERVER || r2s_services_role(1u) != R2S_SVC_CLIENT) {
    finish(4u);
  }
  /* 11 fixed bytes, both length varints and both names: the string walk the
   * announce sizes. */
  if (r2s_services_declare_len(0u) != 38u) {
    finish(5u);
  }
  if (r2s_services_copy(0u, &entry) != R2S_OK ||
    strcmp(entry.type_name, "std_srvs/srv/Trigger") != 0 ||
    strcmp(entry.service_name, "reset") != 0)
  {
    finish(6u);
  }
  r2s_init();
  captured_len = 0u;
  r2s_spin_some();
  if (captured_len != (uint16_t)sizeof declare || memcmp(captured, declare, sizeof declare) != 0) {
    finish(7u);
  }
  finish(0u);
  return 0;
}
