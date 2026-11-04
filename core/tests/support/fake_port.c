/* The scriptable fake port: r2s_port_* backed by in-memory RX and TX buffers,
 * a settable tick and tx_free. */
#include "fake_port.h"

#include <string.h>

#include "bytes.h"
#include "r2s/r2s_port.h"

#define TX_UNLIMITED 0xFFFFu

static struct {
    uint8_t rx[R2S_MAX_LINKS][FAKE_PORT_CAPACITY];
    uint16_t rx_len[R2S_MAX_LINKS];
    uint16_t rx_pos[R2S_MAX_LINKS];
    uint8_t tx[FAKE_PORT_CAPACITY];
    uint16_t tx_len;
    uint16_t tx_write_calls;
    uint16_t tx_write_max;
    uint16_t tx_free;
    uint16_t tx_overruns;
    uint32_t tick[R2S_MAX_LINKS];
} port;

void fake_port_reset(void)
{
    memset(&port, 0, sizeof port);
    port.tx_free = TX_UNLIMITED;
}

void fake_port_queue_rx_link(uint8_t link, const uint8_t *data, uint16_t len)
{
    if (port.rx_pos[link] == port.rx_len[link]) {
        port.rx_pos[link] = port.rx_len[link] = 0;
    }
    if (len > FAKE_PORT_CAPACITY - port.rx_len[link]) {
        fake_port_fail("fake port: RX queue full");
        return;
    }
    if (len > 0u) {
        memcpy(port.rx[link] + port.rx_len[link], data, len);
    }
    port.rx_len[link] = (uint16_t)(port.rx_len[link] + len);
}

void fake_port_queue_rx(const uint8_t *data, uint16_t len)
{
    fake_port_queue_rx_link(0u, data, len);
}

void fake_port_queue_rx_hex(const char *hex)
{
    static uint8_t bytes[FAKE_PORT_CAPACITY];
    int32_t n = r2s_hex_parse(hex, bytes, (uint16_t)sizeof bytes);

    if (n < 0) {
        fake_port_fail("fake port: bad hex for fake_port_queue_rx_hex");
        return;
    }
    fake_port_queue_rx(bytes, (uint16_t)n);
}

uint16_t fake_port_rx_pending_link(uint8_t link)
{
    return (uint16_t)(port.rx_len[link] - port.rx_pos[link]);
}

uint16_t fake_port_rx_pending(void)
{
    return fake_port_rx_pending_link(0u);
}

const uint8_t *fake_port_tx(uint16_t *len)
{
    *len = port.tx_len;
    return port.tx;
}

void fake_port_tx_clear(void)
{
    port.tx_len = 0;
    port.tx_write_calls = 0;
    port.tx_write_max = 0;
}

uint16_t fake_port_tx_write_calls(void)
{
    return port.tx_write_calls;
}

uint16_t fake_port_tx_write_max(void)
{
    return port.tx_write_max;
}

uint16_t fake_port_tx_len(void)
{
    return port.tx_len;
}

uint16_t fake_port_tx_frame_count(void)
{
    uint16_t count = 0;

    for (uint16_t i = 0; i < port.tx_len; i++) {
        count = (uint16_t)(count + (port.tx[i] == 0x00));
    }
    return count;
}

const uint8_t *fake_port_tx_frame(uint16_t index, uint16_t *len)
{
    uint16_t start = 0;

    for (uint16_t i = 0; i < port.tx_len; i++) {
        if (port.tx[i] != 0x00) {
            continue;
        }
        if (index == 0) {
            *len = (uint16_t)(i - start);
            return port.tx + start;
        }
        index--;
        start = (uint16_t)(i + 1);
    }
    *len = 0;
    return NULL;
}

void fake_port_set_tick_link(uint8_t link, uint32_t tick)
{
    port.tick[link] = tick;
}

void fake_port_set_tick(uint32_t tick)
{
    fake_port_set_tick_link(0u, tick);
}

void fake_port_advance(uint32_t ticks)
{
    port.tick[0] += ticks;
}

void fake_port_set_tx_free(uint16_t free_bytes)
{
    port.tx_free = free_bytes;
}

uint16_t fake_port_take_overruns(void)
{
    uint16_t n = port.tx_overruns;

    port.tx_overruns = 0;
    return n;
}

static uint16_t read_from(uint8_t link, uint8_t *buf, uint16_t max)
{
    uint16_t n = fake_port_rx_pending_link(link);

    if (n > max) {
        n = max;
    }
    if (n > 0u) {
        memcpy(buf, port.rx[link] + port.rx_pos[link], n);
    }
    port.rx_pos[link] = (uint16_t)(port.rx_pos[link] + n);
    return n;
}

#if R2S_MAX_LINKS > 1
uint16_t r2s_port_read_link(uint8_t link, uint8_t *buf, uint16_t max)
{
    return read_from(link, buf, max);
}
#endif

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    return read_from(0u, buf, max);
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    port.tx_write_calls++;
    if (len > port.tx_write_max) {
        port.tx_write_max = len;
    }
    /* Bytes past tx_free are still captured, so a failing test shows them. */
    if (port.tx_free != TX_UNLIMITED) {
        if (len > port.tx_free) {
            port.tx_overruns++;
            port.tx_free = 0;
        } else {
            port.tx_free = (uint16_t)(port.tx_free - len);
        }
    }
    if (len > FAKE_PORT_CAPACITY - port.tx_len) {
        fake_port_fail("fake port: TX capture full; fake_port_tx_clear() between phases");
        return;
    }
    if (len > 0u) {
        memcpy(port.tx + port.tx_len, buf, len);
    }
    port.tx_len = (uint16_t)(port.tx_len + len);
}

uint16_t r2s_port_tx_free(void)
{
    return port.tx_free;
}

#if R2S_MAX_LINKS > 1
uint32_t r2s_port_tick_link(uint8_t link)
{
    return port.tick[link];
}
#endif

uint32_t r2s_port_tick(void)
{
    return port.tick[0u];
}
