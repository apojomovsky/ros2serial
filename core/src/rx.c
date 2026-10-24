/* rx: the optional receive ring, compiled only with R2S_WITH_RX_RING: bytes in
 * from the UART interrupt, out through the r2s_port_read() the core provides.
 * Stub until implemented. */
#include "r2s/r2s_rx.h"

#include "r2s/r2s_port.h"

#ifdef R2S_WITH_RX_RING

void r2s_rx_push(uint8_t byte)
{
    (void)byte;
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    (void)buf;
    (void)max;
    return 0;
}

#ifdef R2S_TESTING
void r2s_rx_test_reset(void)
{
}
#endif

#endif
