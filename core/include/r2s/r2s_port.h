/* The port contract: the only boundary between the core and the hardware. Each
 * port implements these as ordinary functions, resolved at link time, so the
 * core needs no function pointers (docs/00-design.md, Port contract). */
#ifndef R2S_PORT_H
#define R2S_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How many links one process serves: 1 on every MCU build, more on a host
 * that frames several serial ports in one process (#122). A build that sets
 * it above 1 also implements r2s_port_read_link() and r2s_port_tick_link();
 * with 1 the framer reaches every port through the two functions below, and
 * no port implements anything new. */
#ifndef R2S_MAX_LINKS
#define R2S_MAX_LINKS 1
#endif

#if R2S_MAX_LINKS < 1
#error "R2S_MAX_LINKS is at least 1"
#endif
#if R2S_MAX_LINKS > 255
#error "R2S_MAX_LINKS fits in the link id, so it is at most 255"
#endif

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

#if R2S_MAX_LINKS > 1
/* The same two functions with the link named, so one process can frame
 * several ports. The link is an id below R2S_MAX_LINKS naming whose bytes
 * and whose tick are read. Only builds with R2S_MAX_LINKS above 1 declare
 * and implement these; single-link builds never name a link. */
uint16_t r2s_port_read_link(uint8_t link, uint8_t *buf, uint16_t max);
uint32_t r2s_port_tick_link(uint8_t link);
#else
/* Single-link builds keep the two functions above as their only seam: the
 * framer names link 0, which these forward to the port. */
static inline uint16_t r2s_port_read_link(uint8_t link, uint8_t *buf, uint16_t max)
{
  (void)link;
  return r2s_port_read(buf, max);
}
static inline uint32_t r2s_port_tick_link(uint8_t link)
{
  (void)link;
  return r2s_port_tick();
}
#endif

#ifdef __cplusplus
}
#endif

#endif
