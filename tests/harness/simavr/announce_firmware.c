/* Announce firmware for the simavr harness: a PROGMEM topic table behind the
 * C13 accessors, announced over UART0. The table lives in flash while its
 * names stay in RAM, as the generator emits them (G7); the session reads
 * every entry byte through R2S_FLASH_READ_BYTE (#189). */

#include <avr/io.h>
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_arch.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"

#define BAUD 115200UL
#define UBRR_VALUE (F_CPU / 16UL / BAUD - 1UL)

/* Two topics with distinct IDs, directions, hashes and names, so the test
 * tells a swapped or truncated announce apart from the right one. */
static const r2s_topic_t R2S_FLASH announce_topics[] = {
    {16u, 0x00u, {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u}, "std_msgs/msg/Float32", "battery"},
    {17u,
     0x01u,
     {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u},
     "geometry_msgs/msg/Twist",
     "cmd_vel"},
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

uint32_t r2s_port_boot_id(void)
{
    return 0xA53C5A3Cu;
}

int main(void)
{
    UBRR0 = (uint16_t)UBRR_VALUE;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
    /* A rejected table still boots and announces what it bound, so the test
     * sees the refusal as a count of zero instead of silence. */
    (void)r2s_session_configure("announce", announce_topics, 2u);
    r2s_init();
    for (;;) {
        r2s_spin_some();
    }
}
