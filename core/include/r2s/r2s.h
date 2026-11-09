// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/* ros2serial core: the public API every firmware includes. */
#ifndef R2S_H
#define R2S_H

#include <stdint.h>

#include "r2s/r2s_arch.h"
#include "r2s/r2s_rx.h"

/// The wire protocol version (spec/protocol.md, GEN-2).
/**
 * A change on the wire that an older peer cannot handle bumps it.
 */
#define R2S_PROTOCOL_VERSION 4

/// What every core call returns: 0 for success, a negative error code.
typedef int8_t r2s_status_t;

/// Success.
#define R2S_OK 0
/// The link is not connected.
#define R2S_ERR_NOT_CONNECTED (-1)
/// A bound the call would exceed.
#define R2S_ERR_BOUNDS (-2)
/// Not enough TX space; nothing was written.
#define R2S_ERR_TX_BUSY (-3)
/// The topic the call names is refused.
#define R2S_ERR_TOPIC_REJECTED (-4)

#ifdef __cplusplus
extern "C" {
#endif

/// Starts the session.
/**
 * Clears the session token, sends HELLO and enters ANNOUNCING (SES-1,
 * SES-10). The table set by r2s_session_configure() or
 * r2s_session_configure_ram() stays; without one the node announces itself
 * nameless with no topics.
 */
void r2s_init(void);

/// Pulls bytes, answers the host, sends heartbeats and watches the link.
/**
 * Up to R2S_SPIN_MAX_FRAMES frames per call, then the timers.
 */
void r2s_spin_some(void);

/// Nonzero while the handshake completed and user traffic may flow.
/**
 * \return Nonzero when connected, 0 otherwise.
 */
uint8_t r2s_connected(void);

#ifdef R2S_WITH_LOG
// Flash-string diagnostics on the log channel (spec/protocol.md, section
// 7.3): one function family per level, one function per value type, so no
// target needs varargs or a format string. Each call sends one frame, or
// drops the line when it does not fit or the transmitter is busy.

// Debug lines (level 10): a plain line, one per value type, and a RAM-text
// line. The value travels in the kind LOG-3 names for its type.
/// Sends a debug line.
/**
 * One frame at level 10, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the flash string naming the line.
 */
void r2s_log_debug(r2s_fstr_t text);
/// Sends a debug line with an unsigned 16-bit value.
/**
 * One frame at level 10, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_debug_u16(r2s_fstr_t text, uint16_t v);
/// Sends a debug line with a signed 16-bit value.
/**
 * One frame at level 10, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_debug_i16(r2s_fstr_t text, int16_t v);
/// Sends a debug line with an unsigned 32-bit value.
/**
 * One frame at level 10, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_debug_u32(r2s_fstr_t text, uint32_t v);
/// Sends a debug line with a signed 32-bit value.
/**
 * One frame at level 10, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_debug_i32(r2s_fstr_t text, int32_t v);
/// Sends a debug line with a 32-bit float value.
/**
 * One frame at level 10, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for f32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_debug_f32(r2s_fstr_t text, float v);
/// Sends a debug line with RAM text.
/**
 * One frame at level 10, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the RAM string naming the line.
 */
void r2s_log_debug_ram(const char *text);

// Info lines (level 20), same shapes as debug.
/// Sends an info line.
/**
 * One frame at level 20, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the flash string naming the line.
 */
void r2s_log_info(r2s_fstr_t text);
/// Sends an info line with an unsigned 16-bit value.
/**
 * One frame at level 20, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_info_u16(r2s_fstr_t text, uint16_t v);
/// Sends an info line with a signed 16-bit value.
/**
 * One frame at level 20, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_info_i16(r2s_fstr_t text, int16_t v);
/// Sends an info line with an unsigned 32-bit value.
/**
 * One frame at level 20, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_info_u32(r2s_fstr_t text, uint32_t v);
/// Sends an info line with a signed 32-bit value.
/**
 * One frame at level 20, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_info_i32(r2s_fstr_t text, int32_t v);
/// Sends an info line with a 32-bit float value.
/**
 * One frame at level 20, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for f32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_info_f32(r2s_fstr_t text, float v);
/// Sends an info line with RAM text.
/**
 * One frame at level 20, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the RAM string naming the line.
 */
void r2s_log_info_ram(const char *text);

// Warn lines (level 30), same shapes as debug.
/// Sends a warn line.
/**
 * One frame at level 30, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the flash string naming the line.
 */
void r2s_log_warn(r2s_fstr_t text);
/// Sends a warn line with an unsigned 16-bit value.
/**
 * One frame at level 30, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_warn_u16(r2s_fstr_t text, uint16_t v);
/// Sends a warn line with a signed 16-bit value.
/**
 * One frame at level 30, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_warn_i16(r2s_fstr_t text, int16_t v);
/// Sends a warn line with an unsigned 32-bit value.
/**
 * One frame at level 30, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_warn_u32(r2s_fstr_t text, uint32_t v);
/// Sends a warn line with a signed 32-bit value.
/**
 * One frame at level 30, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_warn_i32(r2s_fstr_t text, int32_t v);
/// Sends a warn line with a 32-bit float value.
/**
 * One frame at level 30, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for f32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_warn_f32(r2s_fstr_t text, float v);
/// Sends a warn line with RAM text.
/**
 * One frame at level 30, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the RAM string naming the line.
 */
void r2s_log_warn_ram(const char *text);

// Error lines (level 40), same shapes as debug.
/// Sends an error line.
/**
 * One frame at level 40, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the flash string naming the line.
 */
void r2s_log_error(r2s_fstr_t text);
/// Sends an error line with an unsigned 16-bit value.
/**
 * One frame at level 40, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_error_u16(r2s_fstr_t text, uint16_t v);
/// Sends an error line with a signed 16-bit value.
/**
 * One frame at level 40, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_error_i16(r2s_fstr_t text, int16_t v);
/// Sends an error line with an unsigned 32-bit value.
/**
 * One frame at level 40, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_error_u32(r2s_fstr_t text, uint32_t v);
/// Sends an error line with a signed 32-bit value.
/**
 * One frame at level 40, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_error_i32(r2s_fstr_t text, int32_t v);
/// Sends an error line with a 32-bit float value.
/**
 * One frame at level 40, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for f32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_error_f32(r2s_fstr_t text, float v);
/// Sends an error line with RAM text.
/**
 * One frame at level 40, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the RAM string naming the line.
 */
void r2s_log_error_ram(const char *text);

// Fatal lines (level 50), same shapes as debug.
/// Sends a fatal line.
/**
 * One frame at level 50, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the flash string naming the line.
 */
void r2s_log_fatal(r2s_fstr_t text);
/// Sends a fatal line with an unsigned 16-bit value.
/**
 * One frame at level 50, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_fatal_u16(r2s_fstr_t text, uint16_t v);
/// Sends a fatal line with a signed 16-bit value.
/**
 * One frame at level 50, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i16.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_fatal_i16(r2s_fstr_t text, int16_t v);
/// Sends a fatal line with an unsigned 32-bit value.
/**
 * One frame at level 50, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for u32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_fatal_u32(r2s_fstr_t text, uint32_t v);
/// Sends a fatal line with a signed 32-bit value.
/**
 * One frame at level 50, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for i32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_fatal_i32(r2s_fstr_t text, int32_t v);
/// Sends a fatal line with a 32-bit float value.
/**
 * One frame at level 50, or a dropped line when it does not fit or the
 * transmitter is busy. The value travels in the LOG-3 kind for f32.
 *
 * \param[in] text the flash string naming the line.
 * \param[in] v the value to log.
 */
void r2s_log_fatal_f32(r2s_fstr_t text, float v);
/// Sends a fatal line with RAM text.
/**
 * One frame at level 50, or a dropped line when it does not fit or the
 * transmitter is busy.
 *
 * \param[in] text the RAM string naming the line.
 */
void r2s_log_fatal_ram(const char *text);

// The literal forms: they wrap the text so it stays in flash on AVR. Other
// value types call the function with R2S_FSTR(lit) directly.
/// Logs a debug line from a literal.
/**
 * \param[in] lit the literal naming the line.
 */
#define R2S_LOG_DEBUG(lit) r2s_log_debug(R2S_FSTR(lit))
/// Logs a debug line with an unsigned 16-bit value, from a literal.
/**
 * \param[in] lit the literal naming the line.
 * \param[in] v the value to log.
 */
#define R2S_LOG_DEBUG_U16(lit, v) r2s_log_debug_u16(R2S_FSTR(lit), (v))
/// Logs an info line from a literal.
/**
 * \param[in] lit the literal naming the line.
 */
#define R2S_LOG_INFO(lit) r2s_log_info(R2S_FSTR(lit))
/// Logs an info line with an unsigned 16-bit value, from a literal.
/**
 * \param[in] lit the literal naming the line.
 * \param[in] v the value to log.
 */
#define R2S_LOG_INFO_U16(lit, v) r2s_log_info_u16(R2S_FSTR(lit), (v))
/// Logs a warn line from a literal.
/**
 * \param[in] lit the literal naming the line.
 */
#define R2S_LOG_WARN(lit) r2s_log_warn(R2S_FSTR(lit))
/// Logs a warn line with an unsigned 16-bit value, from a literal.
/**
 * \param[in] lit the literal naming the line.
 * \param[in] v the value to log.
 */
#define R2S_LOG_WARN_U16(lit, v) r2s_log_warn_u16(R2S_FSTR(lit), (v))
/// Logs an error line from a literal.
/**
 * \param[in] lit the literal naming the line.
 */
#define R2S_LOG_ERROR(lit) r2s_log_error(R2S_FSTR(lit))
/// Logs an error line with an unsigned 16-bit value, from a literal.
/**
 * \param[in] lit the literal naming the line.
 * \param[in] v the value to log.
 */
#define R2S_LOG_ERROR_U16(lit, v) r2s_log_error_u16(R2S_FSTR(lit), (v))
/// Logs a fatal line from a literal.
/**
 * \param[in] lit the literal naming the line.
 */
#define R2S_LOG_FATAL(lit) r2s_log_fatal(R2S_FSTR(lit))
/// Logs a fatal line with an unsigned 16-bit value, from a literal.
/**
 * \param[in] lit the literal naming the line.
 * \param[in] v the value to log.
 */
#define R2S_LOG_FATAL_U16(lit, v) r2s_log_fatal_u16(R2S_FSTR(lit), (v))

/// Restarts the log seq on every handshake.
/**
 * The session calls this, so the log seq restarts with it ([FRM-4]);
 * firmware never calls it.
 */
void r2s_log_note_connected(void);
#else
// Without R2S_WITH_LOG the literal forms compile out, so one firmware builds
// for tiers with and without logging. The sizeof mentions keep logged values
// used without emitting anything; the + 0 on the value keeps bit-fields
// usable. Literals stay plain, so clang does not see an array decay.
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 */
#define R2S_LOG_DEBUG(lit) ((void)sizeof(lit))
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 * \param[in] v the value, kept used without emitting anything.
 */
#define R2S_LOG_DEBUG_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 */
#define R2S_LOG_INFO(lit) ((void)sizeof(lit))
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 * \param[in] v the value, kept used without emitting anything.
 */
#define R2S_LOG_INFO_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 */
#define R2S_LOG_WARN(lit) ((void)sizeof(lit))
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 * \param[in] v the value, kept used without emitting anything.
 */
#define R2S_LOG_WARN_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 */
#define R2S_LOG_ERROR(lit) ((void)sizeof(lit))
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 * \param[in] v the value, kept used without emitting anything.
 */
#define R2S_LOG_ERROR_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 */
#define R2S_LOG_FATAL(lit) ((void)sizeof(lit))
/// Compiles out without R2S_WITH_LOG.
/**
 * \param[in] lit the literal, kept used without emitting anything.
 * \param[in] v the value, kept used without emitting anything.
 */
#define R2S_LOG_FATAL_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
#endif

#ifdef R2S_WITH_TIME
#include "r2s/r2s_time.h"

/// The MCU's ROS time.
/**
 * The correction's anchor plus the ticks since it (spec/protocol.md,
 * section 7.2).
 *
 * \return The current ROS time, or {0, 0} while no correction anchors the
 * clock.
 */
r2s_time_t r2s_now(void);

/// Nonzero while a correction anchors the clock.
/**
 * \return Nonzero when time is synced, 0 otherwise.
 */
uint8_t r2s_time_synced(void);
#endif

#ifdef R2S_TESTING
/// Returns every core module to its state after power-up.
/**
 * Host test builds only, so each test starts from the same place.
 */
void r2s_test_reset(void);
#endif

#if defined(R2S_WITH_LOG) && defined(R2S_TESTING)
/// Resets the log channel for host test builds.
/**
 * The same reset, as r2s_test_reset() calls it.
 */
void r2s_log_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
