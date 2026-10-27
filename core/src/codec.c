/* codec: the descriptor interpreter (spec/protocol.md, section 9). Stub: every
 * call fails, so the tests stay red until the interpreter lands. */
#include <stddef.h>

#include "r2s/r2s_codec.h"

int8_t r2s_codec_encode(const r2s_codec_table_t *table, uint16_t type, const void *msg, uint8_t narrow,
                        r2s_codec_out_t *out, uint8_t *field)
{
    (void)table;
    (void)type;
    (void)msg;
    (void)narrow;
    if (out != NULL) {
        out->len = 0u;
    }
    if (field != NULL) {
        *field = 0xFFu;
    }
    return R2S_ERR_BOUNDS;
}

int8_t r2s_codec_decode(const r2s_codec_table_t *table, uint16_t type, const uint8_t *payload, uint16_t len,
                        uint8_t narrow, void *msg, uint8_t *field)
{
    (void)table;
    (void)type;
    (void)payload;
    (void)len;
    (void)narrow;
    (void)msg;
    if (field != NULL) {
        *field = 0xFFu;
    }
    return R2S_ERR_DECODE;
}
