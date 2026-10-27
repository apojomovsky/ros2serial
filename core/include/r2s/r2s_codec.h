/* Codec: one iterative interpreter over constant descriptor tables
 * (spec/protocol.md, section 9). It encodes and decodes any message type
 * from type records and field entries in flash, keeping one frame per message
 * level on an explicit stack of R2S_MAX_NEST frames, never recursing. */
#ifndef R2S_CODEC_H
#define R2S_CODEC_H

#include <stdint.h>

#include "r2s/r2s.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Field kinds ([DSC-13]): the .msg type each entry describes. */
#define R2S_CODEC_BOOL 0u
#define R2S_CODEC_U8 1u
#define R2S_CODEC_I8 2u
#define R2S_CODEC_U16 3u
#define R2S_CODEC_I16 4u
#define R2S_CODEC_U32 5u
#define R2S_CODEC_I32 6u
#define R2S_CODEC_U64 7u
#define R2S_CODEC_I64 8u
#define R2S_CODEC_F32 9u
#define R2S_CODEC_F64 10u
#define R2S_CODEC_STR 11u
#define R2S_CODEC_NEST 12u

/* Field modes ([DSC-14]): scalar, fixed array, bounded array. Bit 7 on mode 2
 * says the array's count member is 2 bytes wide instead of 1. */
#define R2S_CODEC_SCALAR 0u
#define R2S_CODEC_FIXED 1u
#define R2S_CODEC_BOUNDED 2u
#define R2S_CODEC_COUNT16 0x80u

/* A payload that breaks an encoding rule. The codec's own code, next to the
 * core's, for what decode refuses without a bound behind it. */
#define R2S_ERR_DECODE (-5)

/* The deepest nesting any table of this build uses, at least 1 ([DSC-39]).
 * The generated configuration sets it; a build without one nests one level. */
#ifndef R2S_MAX_NEST
#define R2S_MAX_NEST 1u
#endif

/* Table placement and byte reads live in r2s_arch.h, so the generator emits
 * the same macros the core reads. */
#include "r2s/r2s_arch.h"

/* The two descriptor tables ([DSC-1]): the type records and the field entries,
 * both little-endian ([DSC-4]) and read through R2S_FLASH_READ_BYTE. */
typedef struct {
    const uint8_t *types;
    uint16_t type_count;
    const uint8_t *fields;
    uint16_t field_count;
} r2s_codec_table_t;

/* Where an encode writes: measure mode (buf NULL, to_tx 0) reports the exact
 * payload length; buffer mode fills buf up to cap; transmit mode streams
 * through r2s_tx_byte. Buffer mode never truncates: a payload that does not
 * fit fails with the bounds error ([DSC-35]). */
typedef struct {
    uint8_t *buf;
    uint16_t cap;
    uint8_t to_tx;
    uint16_t len;
} r2s_codec_out_t;

/* Encodes one message of the table's type into out, in declaration order
 * ([DSC-28]). narrow says the topic the payload goes to is narrow ([ENC-10]).
 * Returns R2S_OK and sets out->len; on failure returns R2S_ERR_BOUNDS, sends
 * nothing on the wire, and names the offending entry in *field ([DSC-37]).
 * The table must be valid ([DSC-41]) and type one of its records. */
int8_t r2s_codec_encode(const r2s_codec_table_t *table, uint16_t type, const void *msg, uint8_t narrow,
                        r2s_codec_out_t *out, uint8_t *field);

/* Decodes one payload into msg, the encode's mirror image. Returns R2S_OK; on
 * failure drops the payload ([DSC-36]) with R2S_ERR_BOUNDS for a bound the
 * message exceeds ([ENC-16]) and R2S_ERR_DECODE for a payload that breaks an
 * encoding rule, naming the offending entry in *field, or 0xFF when no rule
 * names one ([ERR-4]). */
int8_t r2s_codec_decode(const r2s_codec_table_t *table, uint16_t type, const uint8_t *payload, uint16_t len,
                        uint8_t narrow, void *msg, uint8_t *field);

/* Dispatches a user-topic frame received while connected: counts the gap
 * ([FRM-15]), decodes subscribed topics into their receive buffer and calls
 * the topic's handler, and refuses what it cannot dispatch. Defined by the
 * topic configuration: the handwritten table for C9, r2s_generated.c for G8.
 * A build without a topic configuration never calls it (R2S_WITH_CODEC). */
void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
