/* The port contract: the only boundary between the core and the hardware. Each
 * port implements these as ordinary functions, resolved at link time, so the
 * core needs no function pointers (docs/00-design.md, Port contract). */
#ifndef R2S_PORT_H
#define R2S_PORT_H

#include <stdint.h>

/* Copies up to max received bytes into buf without blocking and returns how
 * many it copied, 0 if none are waiting. While assembling a frame the core
 * asks for one byte at a time, so a port never has to buffer past a
 * delimiter for it. With R2S_WITH_RX_RING the core provides this function
 * itself, and the port must not define it. */
uint16_t r2s_port_read(uint8_t *buf, uint16_t max);

/* Sends len bytes. After asking r2s_port_tx_free(), the core writes at most
 * that many bytes in total, in one or more calls, before it asks again; the
 * port accepts them without blocking unless it reported 0xFFFF. */
void r2s_port_write(const uint8_t *buf, uint16_t len);

/* Returns how many bytes r2s_port_write() accepts right now without blocking,
 * or 0xFFFF when the port may block and any length is fine. */
uint16_t r2s_port_tx_free(void);

/* Returns a free-running tick counter that wraps at 2^32, at R2S_TICK_HZ. */
uint32_t r2s_port_tick(void);

/* Returns a value that differs on every boot (an EEPROM counter, or random),
 * so the host can tell a reset from a reconnect. */
uint32_t r2s_port_boot_id(void);

#endif
