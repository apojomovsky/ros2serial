/* ros2serial core: the public API every firmware includes. */
#ifndef R2S_H
#define R2S_H

#include <stdint.h>

#include "r2s/r2s_arch.h"
#include "r2s/r2s_rx.h"

/* The wire protocol version (spec/protocol.md, GEN-2). A change on the wire
 * that an older peer cannot handle bumps it. */
#define R2S_PROTOCOL_VERSION 2

/* What every core call returns: 0 for success, a negative error code. */
typedef int8_t r2s_status_t;

#define R2S_OK 0
#define R2S_ERR_NOT_CONNECTED (-1)
#define R2S_ERR_BOUNDS (-2)
#define R2S_ERR_TX_BUSY (-3) /* not enough TX space; nothing was written */
#define R2S_ERR_TOPIC_REJECTED (-4)

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the session: clears the session token, sends HELLO and enters
 * ANNOUNCING (SES-1, SES-10). The table set by r2s_session_configure() or
 * r2s_session_configure_ram() stays; without one the node announces itself
 * nameless with no topics. */
void r2s_init(void);

/* Pulls bytes, answers the host, sends heartbeats and watches the link: up to
 * R2S_SPIN_MAX_FRAMES frames per call, then the timers. */
void r2s_spin_some(void);

/* Nonzero while the handshake completed and user traffic may flow. */
uint8_t r2s_connected(void);

#ifdef R2S_WITH_LOG
/* Flash-string diagnostics on the log channel (spec/protocol.md, section
 * 7.3): one function family per level, one function per value type, so no
 * target needs varargs or a format string. Each call sends one frame, or
 * drops the line when it does not fit or the transmitter is busy. */

/* Debug lines (level 10): a plain line, one per value type, and a RAM-text
 * line. The value travels in the kind LOG-3 names for its type. */
void r2s_log_debug(r2s_fstr_t text);
void r2s_log_debug_u16(r2s_fstr_t text, uint16_t v);
void r2s_log_debug_i16(r2s_fstr_t text, int16_t v);
void r2s_log_debug_u32(r2s_fstr_t text, uint32_t v);
void r2s_log_debug_i32(r2s_fstr_t text, int32_t v);
void r2s_log_debug_f32(r2s_fstr_t text, float v);
void r2s_log_debug_ram(const char *text);

/* Info lines (level 20), same shapes as debug. */
void r2s_log_info(r2s_fstr_t text);
void r2s_log_info_u16(r2s_fstr_t text, uint16_t v);
void r2s_log_info_i16(r2s_fstr_t text, int16_t v);
void r2s_log_info_u32(r2s_fstr_t text, uint32_t v);
void r2s_log_info_i32(r2s_fstr_t text, int32_t v);
void r2s_log_info_f32(r2s_fstr_t text, float v);
void r2s_log_info_ram(const char *text);

/* Warn lines (level 30), same shapes as debug. */
void r2s_log_warn(r2s_fstr_t text);
void r2s_log_warn_u16(r2s_fstr_t text, uint16_t v);
void r2s_log_warn_i16(r2s_fstr_t text, int16_t v);
void r2s_log_warn_u32(r2s_fstr_t text, uint32_t v);
void r2s_log_warn_i32(r2s_fstr_t text, int32_t v);
void r2s_log_warn_f32(r2s_fstr_t text, float v);
void r2s_log_warn_ram(const char *text);

/* Error lines (level 40), same shapes as debug. */
void r2s_log_error(r2s_fstr_t text);
void r2s_log_error_u16(r2s_fstr_t text, uint16_t v);
void r2s_log_error_i16(r2s_fstr_t text, int16_t v);
void r2s_log_error_u32(r2s_fstr_t text, uint32_t v);
void r2s_log_error_i32(r2s_fstr_t text, int32_t v);
void r2s_log_error_f32(r2s_fstr_t text, float v);
void r2s_log_error_ram(const char *text);

/* Fatal lines (level 50), same shapes as debug. */
void r2s_log_fatal(r2s_fstr_t text);
void r2s_log_fatal_u16(r2s_fstr_t text, uint16_t v);
void r2s_log_fatal_i16(r2s_fstr_t text, int16_t v);
void r2s_log_fatal_u32(r2s_fstr_t text, uint32_t v);
void r2s_log_fatal_i32(r2s_fstr_t text, int32_t v);
void r2s_log_fatal_f32(r2s_fstr_t text, float v);
void r2s_log_fatal_ram(const char *text);

/* The literal forms: they wrap the text so it stays in flash on AVR. Other
 * value types call the function with R2S_FSTR(lit) directly. */
#define R2S_LOG_DEBUG(lit) r2s_log_debug(R2S_FSTR(lit))
#define R2S_LOG_DEBUG_U16(lit, v) r2s_log_debug_u16(R2S_FSTR(lit), (v))
#define R2S_LOG_INFO(lit) r2s_log_info(R2S_FSTR(lit))
#define R2S_LOG_INFO_U16(lit, v) r2s_log_info_u16(R2S_FSTR(lit), (v))
#define R2S_LOG_WARN(lit) r2s_log_warn(R2S_FSTR(lit))
#define R2S_LOG_WARN_U16(lit, v) r2s_log_warn_u16(R2S_FSTR(lit), (v))
#define R2S_LOG_ERROR(lit) r2s_log_error(R2S_FSTR(lit))
#define R2S_LOG_ERROR_U16(lit, v) r2s_log_error_u16(R2S_FSTR(lit), (v))
#define R2S_LOG_FATAL(lit) r2s_log_fatal(R2S_FSTR(lit))
#define R2S_LOG_FATAL_U16(lit, v) r2s_log_fatal_u16(R2S_FSTR(lit), (v))

/* The session calls this on every handshake, so the log seq restarts with it
 * ([FRM-4]); firmware never calls it. */
void r2s_log_note_connected(void);
#else
/* Without R2S_WITH_LOG the literal forms compile out, so one firmware builds
 * for tiers with and without logging. The sizeof mentions keep logged values
 * used without emitting anything; the + 0 on the value keeps bit-fields
 * usable. Literals stay plain, so clang does not see an array decay. */
#define R2S_LOG_DEBUG(lit) ((void)sizeof(lit))
#define R2S_LOG_DEBUG_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
#define R2S_LOG_INFO(lit) ((void)sizeof(lit))
#define R2S_LOG_INFO_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
#define R2S_LOG_WARN(lit) ((void)sizeof(lit))
#define R2S_LOG_WARN_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
#define R2S_LOG_ERROR(lit) ((void)sizeof(lit))
#define R2S_LOG_ERROR_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
#define R2S_LOG_FATAL(lit) ((void)sizeof(lit))
#define R2S_LOG_FATAL_U16(lit, v) ((void)sizeof(lit), (void)sizeof((v) + 0))
#endif

#ifdef R2S_TESTING
/* Host test builds only: returns every core module to its state after
 * power-up, so each test starts from the same place. */
void r2s_test_reset(void);
#endif

#if defined(R2S_WITH_LOG) && defined(R2S_TESTING)
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_log_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
