/* The scriptable fake port the core's tests link instead of a real one. It
 * implements r2s_port_* from r2s/r2s_port.h; these controls drive it. */
#ifndef R2S_TEST_FAKE_PORT_H
#define R2S_TEST_FAKE_PORT_H

#include <stdint.h>

#define FAKE_PORT_CAPACITY 4096u

/* Empties RX and TX, sets the tick and boot ID to 0 and tx_free to 0xFFFF
 * (blocking allowed). Called before every test. */
void fake_port_reset(void);

/* Queues bytes for r2s_port_read(); queuing past the capacity fails the test. */
void fake_port_queue_rx(const uint8_t *data, uint16_t len);
void fake_port_queue_rx_hex(const char *hex);
uint16_t fake_port_rx_pending(void);

/* Everything written so far, and the same split at each 0x00 delimiter. A frame
 * is returned without its delimiter; an unterminated tail is not a frame. */
const uint8_t *fake_port_tx(uint16_t *len);
uint16_t fake_port_tx_len(void);
uint16_t fake_port_tx_frame_count(void);
const uint8_t *fake_port_tx_frame(uint16_t index, uint16_t *len);

/* r2s_port_tick() returns the set value; advance adds to it, wrapping. */
void fake_port_set_tick(uint32_t tick);
void fake_port_advance(uint32_t ticks);

/* r2s_port_tx_free() returns this, reduced by each write; 0xFFFF never shrinks.
 * A write longer than the reported space is counted as an overrun. */
void fake_port_set_tx_free(uint16_t free_bytes);
uint16_t fake_port_tx_overruns(void);

void fake_port_set_boot_id(uint32_t boot_id);

#endif
