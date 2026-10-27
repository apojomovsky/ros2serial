/* log: flash-string diagnostics on the log channel (spec/protocol.md, section
 * 7.3). Each call sends one frame with the level, the text and the value. */
#include "r2s/r2s.h"

#ifdef R2S_WITH_LOG

/* One family per level: a plain line, one per value type, and a RAM-text
 * line. The stub sends nothing; the implementation fills these in. */
#define R2S_LOG_FAMILY(level)                                                                                \
    void r2s_log_##level(r2s_fstr_t text)                                                                    \
    {                                                                                                        \
        (void)text;                                                                                          \
    }                                                                                                        \
    void r2s_log_##level##_u16(r2s_fstr_t text, uint16_t v)                                                  \
    {                                                                                                        \
        (void)text;                                                                                          \
        (void)v;                                                                                             \
    }                                                                                                        \
    void r2s_log_##level##_i16(r2s_fstr_t text, int16_t v)                                                   \
    {                                                                                                        \
        (void)text;                                                                                          \
        (void)v;                                                                                             \
    }                                                                                                        \
    void r2s_log_##level##_u32(r2s_fstr_t text, uint32_t v)                                                  \
    {                                                                                                        \
        (void)text;                                                                                          \
        (void)v;                                                                                             \
    }                                                                                                        \
    void r2s_log_##level##_i32(r2s_fstr_t text, int32_t v)                                                   \
    {                                                                                                        \
        (void)text;                                                                                          \
        (void)v;                                                                                             \
    }                                                                                                        \
    void r2s_log_##level##_f32(r2s_fstr_t text, float v)                                                     \
    {                                                                                                        \
        (void)text;                                                                                          \
        (void)v;                                                                                             \
    }                                                                                                        \
    void r2s_log_##level##_ram(const char *text)                                                             \
    {                                                                                                        \
        (void)text;                                                                                          \
    }

R2S_LOG_FAMILY(debug)
R2S_LOG_FAMILY(info)
R2S_LOG_FAMILY(warn)
R2S_LOG_FAMILY(error)
R2S_LOG_FAMILY(fatal)

void r2s_log_note_connected(void)
{
}

#ifdef R2S_TESTING
void r2s_log_test_reset(void)
{
}
#endif
#else
typedef int r2s_log_unused;
#endif
