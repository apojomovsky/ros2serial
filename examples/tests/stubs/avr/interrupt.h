/* Host stub of avr/interrupt.h for the cmake example test: the interrupt
 * never fires on the host, so the flag macros do nothing. */
#ifndef AVR_INTERRUPT_H
#define AVR_INTERRUPT_H

#define ISR(vector)                                                                                          \
    void vector(void);                                                                                       \
    void vector(void)

#define sei()                                                                                                \
    do {                                                                                                     \
    } while (0)
#define cli()                                                                                                \
    do {                                                                                                     \
    } while (0)

#endif
