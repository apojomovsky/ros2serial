/* framer: bytes in through r2s_port_read, decoded frames out. It cuts frames
 * on 0x00, decodes rCOBS in place and checks the CRC (spec/protocol.md,
 * sections 2 to 4). The core's session dispatches what it delivers, and the
 * bridge links the same code for its side of the link. */
#ifndef R2S_FRAMER_H
#define R2S_FRAMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest decoded frame the receiver accepts, from topic_id to crc16
 * (FRM-9). The generated configuration sets it; a build without one gets a
 * frame that fits a 2 KB part. The protocol limit keeps the encoded frame and
 * its delimiter inside 65535 bytes. */
#ifndef R2S_MAX_FRAME
#define R2S_MAX_FRAME 128
#endif

#if R2S_MAX_FRAME < 4
#error "R2S_MAX_FRAME must hold topic_id, seq and crc16 (FRM-7)"
#endif
#if R2S_MAX_FRAME > 65277
#error "R2S_MAX_FRAME is at most 65277 (FRM-9)"
#endif

/* The receive buffer: the most encoded bytes a frame at the limit takes
 * (COB-8). This is the capacity the handshake announces as rx_capacity
 * (CTL-7). */
#define R2S_FRAMER_RX_CAPACITY (R2S_MAX_FRAME + R2S_MAX_FRAME / 254 + 1)

/* A decoded frame, as [FRM-2] lays it out, and the port tick at which its
 * delimiter arrived, which time sync uses (TIM-1). */
typedef struct {
    const uint8_t *data;
    uint16_t len;
    uint32_t tick;
} r2s_frame_t;

/* The receive counters the heartbeat carries (HB-2), in its order. Each one
 * wraps at 2^16, and a drop counts in every counter its rule names (HB-3). */
typedef struct {
    uint16_t rx_frames;      /* frames accepted (HB-4) */
    uint16_t crc_errors;     /* frames dropped for a bad CRC (HB-5) */
    uint16_t rx_overflows;   /* frames dropped for the buffer bound (HB-6) */
    uint16_t dropped_frames; /* every frame dropped, whichever rule (HB-7) */
} r2s_framer_counters_t;

/* Returns the framer to its power-up state: as if a 0x00 had just arrived
 * (FRM-13), with no frame waiting and the counters at zero. */
void r2s_framer_init(void);

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_framer_test_reset(void);
#endif

/* Pulls bytes from the port one at a time until a frame is decoded or no byte
 * is left, dropping and counting the frames the rules refuse. Returns 1 and
 * fills *frame when one is ready, 0 when none is. While a ready frame has not
 * been released it reads no byte, so the next frame stays in the port and the
 * one waiting for its consumer is never overwritten. */
uint8_t r2s_framer_poll(r2s_frame_t *frame);

/* Releases the ready frame so that the next poll may start a new one. Apart
 * from clearing it, a poll before the release returns the same frame again. */
void r2s_framer_release(void);

/* Returns the counters, which only r2s_framer_init() resets. The session
 * reads them for the heartbeat. */
const r2s_framer_counters_t *r2s_framer_counters(void);

/* Counts a frame the session refused ([SYS-5]): the framer accepted it, but
 * the session took no other action on it. The heartbeat reports it in
 * dropped_frames. */
void r2s_framer_count_refused(void);

#ifdef __cplusplus
}
#endif

#endif
