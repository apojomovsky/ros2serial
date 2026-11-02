/* Host stub of avr/io.h for the cmake example test: plain globals stand
 * in for the registers, which the renamed port functions never touch. */
#ifndef AVR_IO_H
#define AVR_IO_H

#include <stdint.h>

uint16_t UBRR0;
uint8_t UCSR0A;
uint8_t UCSR0B;
uint8_t UCSR0C;
uint8_t UDR0;
uint8_t TCCR0A;
uint8_t TCCR0B;
uint8_t OCR0A;
uint8_t TIMSK0;
uint8_t SREG;

#define RXC0 7u
#define UDRE0 5u
#define RXEN0 4u
#define TXEN0 3u
#define UCSZ01 2u
#define UCSZ00 1u
#define WGM01 1u
#define CS01 1u
#define CS00 0u
#define OCIE0A 1u

#endif
