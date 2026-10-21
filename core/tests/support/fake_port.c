/* Stubs until the fake port is implemented. */
#include "fake_port.h"

#include "r2s/r2s_port.h"

void fake_port_reset(void)
{
}
void fake_port_queue_rx(const uint8_t *data, uint16_t len)
{
    (void)data;
    (void)len;
}
void fake_port_queue_rx_hex(const char *hex)
{
    (void)hex;
}
uint16_t fake_port_rx_pending(void)
{
    return 0;
}
const uint8_t *fake_port_tx(uint16_t *len)
{
    *len = 0;
    return 0;
}
uint16_t fake_port_tx_len(void)
{
    return 0;
}
uint16_t fake_port_tx_frame_count(void)
{
    return 0;
}
const uint8_t *fake_port_tx_frame(uint16_t index, uint16_t *len)
{
    (void)index;
    *len = 0;
    return 0;
}
void fake_port_set_tick(uint32_t tick)
{
    (void)tick;
}
void fake_port_advance(uint32_t ticks)
{
    (void)ticks;
}
void fake_port_set_tx_free(uint16_t free_bytes)
{
    (void)free_bytes;
}
uint16_t fake_port_tx_overruns(void)
{
    return 0;
}
void fake_port_set_boot_id(uint32_t boot_id)
{
    (void)boot_id;
}
uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    (void)buf;
    (void)max;
    return 0;
}
void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    (void)buf;
    (void)len;
}
uint16_t r2s_port_tx_free(void)
{
    return 0;
}
uint32_t r2s_port_tick(void)
{
    return 0;
}
uint32_t r2s_port_boot_id(void)
{
    return 0;
}
