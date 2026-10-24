/* rx: the optional receive ring, compiled only with R2S_WITH_RX_RING: bytes in
 * from the UART interrupt, out through the r2s_port_read() the core provides. */
#include "r2s/r2s_rx.h"

#include "r2s/r2s_port.h"

#ifdef R2S_WITH_RX_RING

#define MASK (R2S_RX_RING_SIZE - 1)

/* Both counters run free over the whole byte range and wrap, so one byte of
 * their difference counts the unread bytes without any wider read, which an
 * 8-bit target cannot do atomically while the other side writes. */
static struct {
    uint8_t buf[R2S_RX_RING_SIZE];
    volatile uint8_t head; /* written by r2s_rx_push() only */
    volatile uint8_t tail; /* written by r2s_port_read() only */
} rx;

void r2s_rx_push(uint8_t byte)
{
    uint8_t used = (uint8_t)(rx.head - rx.tail);

    if (used >= R2S_RX_RING_CAPACITY) {
        return;
    }
    rx.buf[rx.head & MASK] = byte;
    R2S_MEMORY_BARRIER();
    rx.head = (uint8_t)(rx.head + 1u);
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    uint8_t head = rx.head;
    uint8_t tail = rx.tail;
    uint16_t n = 0;

    while (n < max && tail != head) {
        buf[n] = rx.buf[tail & MASK];
        n++;
        tail = (uint8_t)(tail + 1u);
        /* The slot is free only once its byte is copied out: the producer may
         * overwrite it as soon as tail moves. */
        R2S_MEMORY_BARRIER();
        rx.tail = tail;
    }
    return n;
}

#endif /* R2S_WITH_RX_RING */

/* Both builds of this file define it here, so a coverage report that merges
 * them sees one function; without the ring there is nothing to clear. */
#ifdef R2S_TESTING
void r2s_rx_test_reset(void)
{
#ifdef R2S_WITH_RX_RING
    rx.head = 0u;
    rx.tail = 0u;
#endif
}
#endif
