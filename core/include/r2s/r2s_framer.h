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

/// The largest decoded frame the receiver accepts, from topic_id to crc16 (FRM-9).
/**
 * The generated configuration sets it; a build without one gets a frame that
 * fits a 2 KB part. The protocol limit keeps the encoded frame and its
 * delimiter inside 65535 bytes.
 */
#ifndef R2S_MAX_FRAME
#define R2S_MAX_FRAME 128
#endif

#if R2S_MAX_FRAME < 4
#error "R2S_MAX_FRAME must hold topic_id, seq and crc16 (FRM-7)"
#endif
#if R2S_MAX_FRAME > 65277
#error "R2S_MAX_FRAME is at most 65277 (FRM-9)"
#endif

/// The receive buffer.
/**
 * The most encoded bytes a frame at the limit takes (COB-8). This is the
 * capacity the handshake announces as rx_capacity (CTL-7).
 */
#define R2S_FRAMER_RX_CAPACITY (R2S_MAX_FRAME + R2S_MAX_FRAME / 254 + 1)

/// A decoded frame, as [FRM-2] lays it out.
typedef struct
{
  const uint8_t *data;  ///< The decoded frame bytes.
  uint16_t len;         ///< How many bytes data holds.
  uint32_t tick;        ///< The tick at which the delimiter arrived (TIM-1).
} r2s_frame_t;

/// The receive counters the heartbeat carries (HB-2), in its order.
/**
 * Each one wraps at 2^16, and a drop counts in every counter its rule
 * names (HB-3).
 */
typedef struct
{
  uint16_t rx_frames;       ///< Frames accepted (HB-4).
  uint16_t crc_errors;      ///< Frames dropped for a bad CRC (HB-5).
  uint16_t rx_overflows;    ///< Frames dropped for the buffer bound (HB-6).
  uint16_t dropped_frames;  ///< Every frame dropped, whichever rule (HB-7).
} r2s_framer_counters_t;

/// One link's framer state.
/**
 * The receive buffer, the frame in progress, the decoded frame waiting for
 * its release, the drops its session reports on the error channel, and the
 * heartbeat counters. The caller owns the storage, one struct per link, so
 * several links frame in one process, each reading its own link from the
 * port seam (#122). The tag lets C++ name the struct without this header.
 */
typedef struct r2s_framer
{
  uint8_t link;                          ///< Whose bytes and tick this instance reads.
  uint8_t rx[R2S_FRAMER_RX_CAPACITY];    ///< The encoded bytes received so far.
  uint16_t len;                          ///< How many bytes rx holds.
  uint8_t overflow;                      ///< Nonzero while the frame in progress overran.
  uint8_t ready;                         ///< Nonzero while a decoded frame waits.
  const uint8_t *ready_data;             ///< The waiting frame's first byte.
  uint16_t ready_len;                    ///< The waiting frame's length.
  uint32_t ready_tick;                   ///< The tick at which its delimiter arrived.
  uint16_t rep_overflow;                 ///< Receive overflows since the last take.
  uint16_t rep_toolarge;                 ///< Frames past the limit since the last take.
  uint8_t rep_topic;                     ///< The last too-large frame's topic.
  r2s_framer_counters_t counters;        ///< The heartbeat counters.
} r2s_framer_t;

/// Returns the framer to its power-up state.
/**
 * As if a 0x00 had just arrived (FRM-13), with no frame waiting and the
 * counters at zero.
 */
void r2s_framer_init(void);

/// Resets one owned instance, bound to link.
/**
 * The same reset as r2s_framer_init(). The singleton above is the default
 * instance on link 0.
 *
 * \param[out] framer the instance to reset.
 * \param[in] link whose bytes and tick the instance reads.
 * \pre link is below R2S_MAX_LINKS.
 */
void r2s_framer_init_inst(r2s_framer_t *framer, uint8_t link);

#ifdef R2S_TESTING
/// Resets the framer for host test builds.
/**
 * The same reset, as r2s_test_reset() calls it.
 */
void r2s_framer_test_reset(void);
#endif

/// Pulls bytes from the port until a frame is decoded or no byte is left.
/**
 * Pulls one byte at a time, dropping and counting the frames the rules
 * refuse. While a ready frame has not been released it reads no byte, so the
 * next frame stays in the port and the one waiting for its consumer is never
 * overwritten.
 *
 * \param[out] frame where to write the decoded frame.
 * \return 1 with *frame filled when one is ready, 0 when none is.
 */
uint8_t r2s_framer_poll(r2s_frame_t *frame);

/// Releases the ready frame so the next poll may start a new one.
/**
 * Apart from clearing it, a poll before the release returns the same frame
 * again.
 */
void r2s_framer_release(void);

/// Polls one owned instance, reading that instance's link.
/**
 * The same poll as r2s_framer_poll(). Ready frames on different instances
 * never alias: each one points into its own instance's buffer.
 *
 * \param[in,out] framer the instance to poll.
 * \param[out] frame where to write the decoded frame.
 * \return 1 with *frame filled when one is ready, 0 when none is.
 */
uint8_t r2s_framer_poll_inst(r2s_framer_t *framer, r2s_frame_t *frame);

/// Releases one owned instance's ready frame.
/**
 * The same release as r2s_framer_release(), for the instance's own link.
 *
 * \param[in,out] framer the instance to release.
 */
void r2s_framer_release_inst(r2s_framer_t *framer);

/// Returns the counters, which only r2s_framer_init() resets.
/**
 * The session reads them for the heartbeat.
 *
 * \return The default instance's counters.
 */
const r2s_framer_counters_t * r2s_framer_counters(void);

/// Returns the counters of one owned instance.
/**
 * Reset only by r2s_framer_init_inst(). The pointer stays valid while the
 * instance outlives its use.
 *
 * \param[in] framer the instance to read.
 * \return The instance's counters.
 */
const r2s_framer_counters_t * r2s_framer_counters_inst(const r2s_framer_t *framer);

/// Counts a frame the session refused ([SYS-5]).
/**
 * The framer accepted it, but the session took no other action on it. The
 * heartbeat reports it in dropped_frames.
 */
void r2s_framer_count_refused(void);

/// Counts a refused frame on one owned instance.
/**
 * The same count as r2s_framer_count_refused(), instead of the default one.
 *
 * \param[in,out] framer the instance to count on.
 */
void r2s_framer_count_refused_inst(r2s_framer_t *framer);

/// Takes the drops the session reports on the error channel.
/**
 * Since the last take: receive overflows, whose bytes never decode, and
 * frames past the limit with the last one's topic. The take clears both, so
 * each occurrence reports once. The bridge links the framer but never takes.
 *
 * \param[out] overflow where to write the receive overflows.
 * \param[out] too_large where to write the frames past the limit.
 * \param[out] topic where to write the last too-large frame's topic.
 */
void r2s_framer_take_error_reports(uint16_t *overflow, uint16_t *too_large, uint8_t *topic);

/// Takes one owned instance's reports instead of the default one's.
/**
 * The same take as r2s_framer_take_error_reports().
 *
 * \param[in,out] framer the instance to take from.
 * \param[out] overflow where to write the receive overflows.
 * \param[out] too_large where to write the frames past the limit.
 * \param[out] topic where to write the last too-large frame's topic.
 */
void r2s_framer_take_error_reports_inst(
  r2s_framer_t *framer, uint16_t *overflow, uint16_t *too_large,
  uint8_t *topic);

#ifdef __cplusplus
}
#endif

#endif
