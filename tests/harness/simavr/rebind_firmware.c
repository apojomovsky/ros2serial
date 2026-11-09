/* Rebind firmware for the simavr harness: a PROGMEM topic table behind the
 * C13 accessors, rebound to a RAM table with different content when the
 * host publishes on the trigger subscription (#233). The flash table binds
 * through the static entry point at boot and the RAM table through the
 * runtime one on trigger, so one run observes the flag following the
 * rebind. The names stay in RAM in both tables. */

#include <avr/io.h>
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_arch.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"

#define BAUD 115200UL
#define UBRR_VALUE (F_CPU / 16UL / BAUD - 1UL)

#define TRIGGER_TOPIC_ID 17u

/* The boot table: two topics with distinct IDs, directions, hashes and
 * names, so a swapped or truncated announce reads apart from the right one.
 * The second row is the trigger subscription the host publishes on. */
static const r2s_topic_t R2S_FLASH flash_topics[] = {
  {16u, 0x00u, {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u}, "std_msgs/msg/Float32",
    "battery"},
  {17u,
    0x01u,
    {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u},
    "geometry_msgs/msg/Twist",
    "cmd_vel"},
};

/* The rebind table: one topic, so the count, the ID, the flags, the hash
 * and both names all differ from the boot table. Plain static storage keeps
 * it in RAM, where the runtime entry point reads it. */
static r2s_topic_t ram_topics[] = {
  {18u,
    0x01u,
    {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u},
    "geometry_msgs/msg/Twist",
    "rebound"},
};

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t n = 0u;

  while (n < max && (UCSR0A & (1u << RXC0)) != 0u) {
    buf[n] = UDR0;
    n++;
  }
  return n;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    loop_until_bit_is_set(UCSR0A, UDRE0);
    UDR0 = buf[i];
  }
}

uint16_t r2s_port_tx_free(void)
{
  return 0xFFFFu;
}

/* The test drives every reply from its own frames, so a fixed tick keeps the
 * announce deterministic: only solicited answers, never a repeat. */
uint32_t r2s_port_tick(void)
{
  return 0u;
}

/* The trigger path: only the trigger subscription rebinds, and only once
 * the host resolved it, so stray frames before the handshake refuse like
 * any other dispatch. The payload carries no signal; any frame on the
 * trigger rebinds. A second trigger rebinds the same table again. */
void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
  uint8_t index;

  (void)payload;
  (void)len;
  if (topic_id != TRIGGER_TOPIC_ID) {
    r2s_framer_count_refused();
    return;
  }
  index = r2s_session_find_topic(topic_id);
  if (index == 0xFFu || r2s_session_topic_status(index) != 0u) {
    r2s_framer_count_refused();
    return;
  }
  (void)r2s_session_note_rx_seq(index, seq);
  (void)r2s_session_configure_ram("rebind", ram_topics, 1u);
}

int main(void)
{
  UBRR0 = (uint16_t)UBRR_VALUE;
  UCSR0B = (1 << RXEN0) | (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
    /* A rejected table still boots and announces what it bound, so the test
     * sees the refusal as a count of zero instead of silence. */
  (void)r2s_session_configure("rebind", flash_topics, 2u);
  r2s_init();
  for (;; ) {
    r2s_spin_some();
  }
}
