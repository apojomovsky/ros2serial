/* Echo firmware for the simavr harness: every byte that arrives on UART0 goes
   back out unchanged. It uses no ros2serial code, so a harness fault never
   looks like a core fault. */

#include <avr/io.h>

#define BAUD 115200UL
#define UBRR_VALUE (F_CPU / 16UL / BAUD - 1UL)

int main(void)
{
    UBRR0 = (uint16_t)UBRR_VALUE;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
    for (;;) {
        loop_until_bit_is_set(UCSR0A, RXC0);
        uint8_t byte = UDR0;
        loop_until_bit_is_set(UCSR0A, UDRE0);
        UDR0 = byte;
    }
}
