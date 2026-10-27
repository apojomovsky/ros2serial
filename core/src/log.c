/* log: flash-string diagnostics on the log channel (spec/protocol.md, section
 * 7.3). Each call sends one frame with the level, the text and the value. */
#include "r2s/r2s.h"

#ifdef R2S_WITH_LOG

#include <string.h>

#ifdef __AVR__
#include <avr/pgmspace.h>
#endif

#include "r2s/r2s_framer.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"

/* The channel the MCU sends log frames on, and the levels and value kinds of
 * LOG-2 and LOG-3. */
#define R2S_LOG_TOPIC 2u

#define R2S_LOG_LEVEL_DEBUG 10u
#define R2S_LOG_LEVEL_INFO 20u
#define R2S_LOG_LEVEL_WARN 30u
#define R2S_LOG_LEVEL_ERROR 40u
#define R2S_LOG_LEVEL_FATAL 50u

#define R2S_LOG_KIND_NONE 0u
#define R2S_LOG_KIND_U16 1u
#define R2S_LOG_KIND_I16 2u
#define R2S_LOG_KIND_U32 3u
#define R2S_LOG_KIND_I32 4u
#define R2S_LOG_KIND_F32 5u

static uint8_t log_seq;

#ifdef __AVR__
/* A flash string lives in PROGMEM, so its length and bytes go through
 * pgmspace; anywhere else they are plain memory reads. */
static size_t fstr_len(r2s_fstr_t text)
{
    return strlen_P(text);
}

static uint8_t fstr_at(r2s_fstr_t text, uint16_t i)
{
    return pgm_read_byte((const uint8_t *)(text + i));
}
#else
static size_t fstr_len(r2s_fstr_t text)
{
    return strlen(text);
}

static uint8_t fstr_at(r2s_fstr_t text, uint16_t i)
{
    return (uint8_t)text[i];
}
#endif

/* The payload length of a line: the level, the varint length and the text,
 * the kind and the value. The caller bounds the text to the varint range, so
 * the sums fit 32 bits on every target. */
static uint32_t frame_len(uint16_t text_len, uint8_t value_len)
{
    uint8_t len_buf[R2S_VARINT_MAX_BYTES];

    return 1u + (uint32_t)r2s_varint_encode(text_len, len_buf) + (uint32_t)text_len + 1u +
           (uint32_t)value_len;
}

/* Sends one line: the level, the text, the kind and the value bytes. The text
 * is non-NULL; the caller drops NULL before measuring it. Drops the line
 * when it does not fit the frame or the transmitter is busy, sending nothing
 * either way. */
static void send_line(uint8_t level, r2s_fstr_t text, uint16_t text_len, uint8_t from_flash, uint8_t kind,
                      uint32_t value, uint8_t value_len)
{
    uint32_t len = frame_len(text_len, value_len);
    uint8_t varint[R2S_VARINT_MAX_BYTES];
    uint8_t varint_len;
    uint16_t i;

    if (len > (uint32_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD)) {
        return;
    }
    if (r2s_tx_begin(R2S_LOG_TOPIC, log_seq, (uint16_t)len) != R2S_OK) {
        return;
    }
    (void)r2s_tx_byte(level);
    varint_len = r2s_varint_encode(text_len, varint);
    for (i = 0u; i < varint_len; i++) {
        (void)r2s_tx_byte(varint[i]);
    }
    for (i = 0u; i < text_len; i++) {
        (void)r2s_tx_byte(from_flash != 0u ? fstr_at(text, i) : (uint8_t)text[i]);
    }
    (void)r2s_tx_byte(kind);
    for (i = 0u; i < value_len; i++) {
        (void)r2s_tx_byte((uint8_t)(value >> (8u * i)));
    }
    if (r2s_tx_end() == R2S_OK) {
        log_seq++;
    }
}

/* A plain flash-string line. */
static void log_plain(uint8_t level, r2s_fstr_t text)
{
    size_t raw;

    if (text == NULL) {
        return;
    }
    raw = fstr_len(text);
    if (raw > (size_t)R2S_VARINT_MAX) {
        return;
    }
    send_line(level, text, (uint16_t)raw, 1u, R2S_LOG_KIND_NONE, 0u, 0u);
}

/* A flash-string line with a value, stored little-endian (GEN-1). */
static void log_value(uint8_t level, r2s_fstr_t text, uint8_t kind, uint32_t value, uint8_t value_len)
{
    size_t raw;

    if (text == NULL) {
        return;
    }
    raw = fstr_len(text);
    if (raw > (size_t)R2S_VARINT_MAX) {
        return;
    }
    send_line(level, text, (uint16_t)raw, 1u, kind, value, value_len);
}

/* A runtime-text line: the text lives in RAM. */
static void log_ram(uint8_t level, const char *text)
{
    size_t raw;

    if (text == NULL) {
        return;
    }
    raw = strlen(text);
    if (raw > (size_t)R2S_VARINT_MAX) {
        return;
    }
    send_line(level, text, (uint16_t)raw, 0u, R2S_LOG_KIND_NONE, 0u, 0u);
}

/* Copies the float's bytes without arithmetic, so the zone check sees no
 * float operation. */
static uint32_t float_bits(float v)
{
    uint32_t bits;

    memcpy(&bits, &v, sizeof bits);
    return bits;
}

void r2s_log_debug(r2s_fstr_t text)
{
    log_plain(R2S_LOG_LEVEL_DEBUG, text);
}

void r2s_log_debug_u16(r2s_fstr_t text, uint16_t v)
{
    log_value(R2S_LOG_LEVEL_DEBUG, text, R2S_LOG_KIND_U16, (uint32_t)v, 2u);
}

void r2s_log_debug_i16(r2s_fstr_t text, int16_t v)
{
    log_value(R2S_LOG_LEVEL_DEBUG, text, R2S_LOG_KIND_I16, (uint32_t)(uint16_t)v, 2u);
}

void r2s_log_debug_u32(r2s_fstr_t text, uint32_t v)
{
    log_value(R2S_LOG_LEVEL_DEBUG, text, R2S_LOG_KIND_U32, v, 4u);
}

void r2s_log_debug_i32(r2s_fstr_t text, int32_t v)
{
    log_value(R2S_LOG_LEVEL_DEBUG, text, R2S_LOG_KIND_I32, (uint32_t)v, 4u);
}

void r2s_log_debug_f32(r2s_fstr_t text, float v)
{
    log_value(R2S_LOG_LEVEL_DEBUG, text, R2S_LOG_KIND_F32, float_bits(v), 4u);
}

void r2s_log_debug_ram(const char *text)
{
    log_ram(R2S_LOG_LEVEL_DEBUG, text);
}

void r2s_log_info(r2s_fstr_t text)
{
    log_plain(R2S_LOG_LEVEL_INFO, text);
}

void r2s_log_info_u16(r2s_fstr_t text, uint16_t v)
{
    log_value(R2S_LOG_LEVEL_INFO, text, R2S_LOG_KIND_U16, (uint32_t)v, 2u);
}

void r2s_log_info_i16(r2s_fstr_t text, int16_t v)
{
    log_value(R2S_LOG_LEVEL_INFO, text, R2S_LOG_KIND_I16, (uint32_t)(uint16_t)v, 2u);
}

void r2s_log_info_u32(r2s_fstr_t text, uint32_t v)
{
    log_value(R2S_LOG_LEVEL_INFO, text, R2S_LOG_KIND_U32, v, 4u);
}

void r2s_log_info_i32(r2s_fstr_t text, int32_t v)
{
    log_value(R2S_LOG_LEVEL_INFO, text, R2S_LOG_KIND_I32, (uint32_t)v, 4u);
}

void r2s_log_info_f32(r2s_fstr_t text, float v)
{
    log_value(R2S_LOG_LEVEL_INFO, text, R2S_LOG_KIND_F32, float_bits(v), 4u);
}

void r2s_log_info_ram(const char *text)
{
    log_ram(R2S_LOG_LEVEL_INFO, text);
}

void r2s_log_warn(r2s_fstr_t text)
{
    log_plain(R2S_LOG_LEVEL_WARN, text);
}

void r2s_log_warn_u16(r2s_fstr_t text, uint16_t v)
{
    log_value(R2S_LOG_LEVEL_WARN, text, R2S_LOG_KIND_U16, (uint32_t)v, 2u);
}

void r2s_log_warn_i16(r2s_fstr_t text, int16_t v)
{
    log_value(R2S_LOG_LEVEL_WARN, text, R2S_LOG_KIND_I16, (uint32_t)(uint16_t)v, 2u);
}

void r2s_log_warn_u32(r2s_fstr_t text, uint32_t v)
{
    log_value(R2S_LOG_LEVEL_WARN, text, R2S_LOG_KIND_U32, v, 4u);
}

void r2s_log_warn_i32(r2s_fstr_t text, int32_t v)
{
    log_value(R2S_LOG_LEVEL_WARN, text, R2S_LOG_KIND_I32, (uint32_t)v, 4u);
}

void r2s_log_warn_f32(r2s_fstr_t text, float v)
{
    log_value(R2S_LOG_LEVEL_WARN, text, R2S_LOG_KIND_F32, float_bits(v), 4u);
}

void r2s_log_warn_ram(const char *text)
{
    log_ram(R2S_LOG_LEVEL_WARN, text);
}

void r2s_log_error(r2s_fstr_t text)
{
    log_plain(R2S_LOG_LEVEL_ERROR, text);
}

void r2s_log_error_u16(r2s_fstr_t text, uint16_t v)
{
    log_value(R2S_LOG_LEVEL_ERROR, text, R2S_LOG_KIND_U16, (uint32_t)v, 2u);
}

void r2s_log_error_i16(r2s_fstr_t text, int16_t v)
{
    log_value(R2S_LOG_LEVEL_ERROR, text, R2S_LOG_KIND_I16, (uint32_t)(uint16_t)v, 2u);
}

void r2s_log_error_u32(r2s_fstr_t text, uint32_t v)
{
    log_value(R2S_LOG_LEVEL_ERROR, text, R2S_LOG_KIND_U32, v, 4u);
}

void r2s_log_error_i32(r2s_fstr_t text, int32_t v)
{
    log_value(R2S_LOG_LEVEL_ERROR, text, R2S_LOG_KIND_I32, (uint32_t)v, 4u);
}

void r2s_log_error_f32(r2s_fstr_t text, float v)
{
    log_value(R2S_LOG_LEVEL_ERROR, text, R2S_LOG_KIND_F32, float_bits(v), 4u);
}

void r2s_log_error_ram(const char *text)
{
    log_ram(R2S_LOG_LEVEL_ERROR, text);
}

void r2s_log_fatal(r2s_fstr_t text)
{
    log_plain(R2S_LOG_LEVEL_FATAL, text);
}

void r2s_log_fatal_u16(r2s_fstr_t text, uint16_t v)
{
    log_value(R2S_LOG_LEVEL_FATAL, text, R2S_LOG_KIND_U16, (uint32_t)v, 2u);
}

void r2s_log_fatal_i16(r2s_fstr_t text, int16_t v)
{
    log_value(R2S_LOG_LEVEL_FATAL, text, R2S_LOG_KIND_I16, (uint32_t)(uint16_t)v, 2u);
}

void r2s_log_fatal_u32(r2s_fstr_t text, uint32_t v)
{
    log_value(R2S_LOG_LEVEL_FATAL, text, R2S_LOG_KIND_U32, v, 4u);
}

void r2s_log_fatal_i32(r2s_fstr_t text, int32_t v)
{
    log_value(R2S_LOG_LEVEL_FATAL, text, R2S_LOG_KIND_I32, (uint32_t)v, 4u);
}

void r2s_log_fatal_f32(r2s_fstr_t text, float v)
{
    log_value(R2S_LOG_LEVEL_FATAL, text, R2S_LOG_KIND_F32, float_bits(v), 4u);
}

void r2s_log_fatal_ram(const char *text)
{
    log_ram(R2S_LOG_LEVEL_FATAL, text);
}

void r2s_log_note_connected(void)
{
    log_seq = 0u;
}

#ifdef R2S_TESTING
void r2s_log_test_reset(void)
{
    log_seq = 0u;
}
#endif
#else
typedef int r2s_log_unused;
#endif
