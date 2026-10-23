/* Deep-call variant of the echo firmware: four nested frames of 48 bytes run
   once before the echo loop, so the harness's stack painting has a depth to
   measure that the echo firmware alone does not reach. */

#include <avr/io.h>

#define BAUD 115200UL
#define UBRR_VALUE (F_CPU / 16UL / BAUD - 1UL)

static volatile uint8_t sink;

/* Recursion keeps the compiler from folding the frames into main. */
static void depth(uint8_t level)
{
    volatile uint8_t frame[48];
    for (uint8_t i = 0; i < sizeof frame; i++) {
        frame[i] = i;
    }
    if (level > 0) {
        depth((uint8_t)(level - 1));
    }
    for (uint8_t i = 0; i < sizeof frame; i++) {
        sink = (uint8_t)(sink + frame[i]);
    }
}

int main(void)
{
    UBRR0 = (uint16_t)UBRR_VALUE;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
    depth(3);
    for (;;) {
        loop_until_bit_is_set(UCSR0A, RXC0);
        uint8_t byte = UDR0;
        loop_until_bit_is_set(UCSR0A, UDRE0);
        UDR0 = byte;
    }
}
