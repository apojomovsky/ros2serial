/* Minimal CMake example firmware: the generated pair on an ATmega328P at
 * 16 MHz, publishing battery and counting cmd_vel frames. */
#include <avr/eeprom.h>
#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s_generated.h"

/* 115200 baud from the 16 MHz crystal: the divisor truncates, keeping the
 * rate error under a percent, which the bridge tolerates. */
#define UBRR_VALUE (16000000UL / 16UL / 115200UL - 1UL)

/* Timer0 compare value for a 1 ms tick: 16 MHz / 64 / 250. */
#define TIMER0_TOP 249u

/* Milliseconds since boot, advanced by the Timer0 compare interrupt. */
static volatile uint32_t s_ms;

/* Timer0 compare interrupt: the firmware's millisecond source. */
ISR(TIMER0_COMPA_vect)
{
    s_ms++;
}

/* Starts UART0 at 115200 8-N-1 and the millisecond tick. */
static void hardware_init(void)
{
    UBRR0 = (uint16_t)UBRR_VALUE;
    UCSR0B = (uint8_t)((1 << RXEN0) | (1 << TXEN0));
    UCSR0C = (uint8_t)((1 << UCSZ01) | (1 << UCSZ00));
    TCCR0A = (uint8_t)(1 << WGM01);
    TCCR0B = (uint8_t)((1 << CS01) | (1 << CS00));
    OCR0A = (uint8_t)TIMER0_TOP;
    TIMSK0 = (uint8_t)(1 << OCIE0A);
    sei();
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    uint16_t count = 0u;

    while (count < max && (UCSR0A & (uint8_t)(1 << RXC0)) != 0u) {
        buf[count] = UDR0;
        count++;
    }
    return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    for (i = 0u; i < len; i++) {
        while ((UCSR0A & (uint8_t)(1 << UDRE0)) == 0u) {
        }
        UDR0 = buf[i];
    }
}

/* The write above polls for space, so it never refuses a byte. */
uint16_t r2s_port_tx_free(void)
{
    return 0xFFFFu;
}

/* The tick as milliseconds: read with interrupts off, since a 32-bit load
 * on AVR runs as four 8-bit loads that an interrupt could split. */
uint32_t r2s_port_tick(void)
{
    uint8_t flags = SREG;
    uint32_t now;

    cli();
    now = s_ms;
    SREG = flags;
    return now;
}

/* A counter in the first EEPROM cell, so every boot announces a new ID. The
 * first boot reads erased 0xFFFFFFFF and announces 0. */
uint32_t r2s_port_boot_id(void)
{
    uint32_t boot_id = eeprom_read_dword((const uint32_t *)0);

    boot_id += 1u;
    eeprom_update_dword((uint32_t *)0, boot_id);
    return boot_id;
}

static uint32_t s_cmd_vel_count;

/* Handles cmd_vel; dispatch calls it from r2s_spin_some(). */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
    s_cmd_vel_count++;
}

int main(void)
{
    uint32_t last_pub;
    uint32_t sent = 0u;

    hardware_init();
    if (r2s_session_configure("hello_cmake", r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return 1;
    }
    r2s_init();
    last_pub = r2s_port_tick();
    for (;;) {
        uint32_t now;

        r2s_spin_some();
        now = r2s_port_tick();
        if (r2s_connected() == 0u) {
            continue;
        }
        if ((uint32_t)(now - last_pub) >= 100u) {
            std_msgs__msg__Float32 msg;

            msg.data = (float)sent;
            if (r2s_publish_battery(&msg) == R2S_OK) {
                sent++;
                last_pub = now;
            }
        }
    }
}
