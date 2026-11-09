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

// Field kinds ([DSC-13]): the .msg type each entry describes.
/// Boolean.
#define R2S_CODEC_BOOL 0u
/// Unsigned 8-bit integer.
#define R2S_CODEC_U8 1u
/// Signed 8-bit integer.
#define R2S_CODEC_I8 2u
/// Unsigned 16-bit integer.
#define R2S_CODEC_U16 3u
/// Signed 16-bit integer.
#define R2S_CODEC_I16 4u
/// Unsigned 32-bit integer.
#define R2S_CODEC_U32 5u
/// Signed 32-bit integer.
#define R2S_CODEC_I32 6u
/// Unsigned 64-bit integer.
#define R2S_CODEC_U64 7u
/// Signed 64-bit integer.
#define R2S_CODEC_I64 8u
/// 32-bit float.
#define R2S_CODEC_F32 9u
/// 64-bit float.
#define R2S_CODEC_F64 10u
/// String.
#define R2S_CODEC_STR 11u
/// Nested message.
#define R2S_CODEC_NEST 12u

// Field modes ([DSC-14]): scalar, fixed array, bounded array. Bit 7 on mode 2
// says the array's count member is 2 bytes wide instead of 1.
/// Scalar field.
#define R2S_CODEC_SCALAR 0u
/// Fixed-length array field.
#define R2S_CODEC_FIXED 1u
/// Bounded array field.
#define R2S_CODEC_BOUNDED 2u
/// Count width flag: the array's count member is 2 bytes wide instead of 1.
#define R2S_CODEC_COUNT16 0x80u

/// A payload that breaks an encoding rule.
/**
 * The codec's own code, next to the core's, for what decode refuses without
 * a bound behind it.
 */
#define R2S_ERR_DECODE (-5)

/// The deepest nesting any table of this build uses, at least 1 ([DSC-39]).
/**
 * The generated configuration sets it; a build without one nests one level.
 */
#ifndef R2S_MAX_NEST
#define R2S_MAX_NEST 1u
#endif

/* Table placement and byte reads live in r2s_arch.h, so the generator emits
 * the same macros the core reads. */
#include "r2s/r2s_arch.h"

/// The two descriptor tables ([DSC-1]).
/**
 * The type records and the field entries, both little-endian ([DSC-4]) and
 * read through R2S_FLASH_READ_BYTE.
 */
typedef struct
{
  const uint8_t *types;  ///< The type records.
  uint16_t type_count;   ///< How many records types holds.
  const uint8_t *fields;  ///< The field entries.
  uint16_t field_count;   ///< How many entries fields holds.
} r2s_codec_table_t;

/// Where an encode writes.
/**
 * Measure mode (buf NULL, to_tx 0) reports the exact payload length; buffer
 * mode fills buf up to cap; transmit mode streams through r2s_tx_byte.
 * Buffer mode never truncates: a payload that does not fit fails with the
 * bounds error ([DSC-35]).
 */
typedef struct
{
  uint8_t *buf;  ///< Where buffer mode writes; NULL in measure mode.
  uint16_t cap;  ///< How many bytes buf holds.
  uint8_t to_tx;  ///< Nonzero to stream through r2s_tx_byte.
  uint16_t len;  ///< The payload length: measured, written or streamed.
} r2s_codec_out_t;

/// Encodes one message of the table's type into out, in declaration order ([DSC-28]).
/**
 * narrow says the topic the payload goes to is narrow ([ENC-10]). On failure
 * it sends nothing on the wire.
 *
 * \param[in] table the descriptor tables to encode with.
 * \param[in] type which record of the table msg has.
 * \param[in] msg the message to encode.
 * \param[in] narrow nonzero when the payload goes to a narrow topic.
 * \param[in,out] out where to write the payload.
 * \param[out] field where to name the offending entry on failure.
 * \return R2S_OK with out->len set, or R2S_ERR_BOUNDS, naming the offending
 * entry in *field ([DSC-37]).
 * \pre The table is valid ([DSC-41]) and type is one of its records.
 */
int8_t r2s_codec_encode(
  const r2s_codec_table_t *table, uint16_t type, const void *msg, uint8_t narrow,
  r2s_codec_out_t *out, uint8_t *field);

/// Decodes one payload into msg, the encode's mirror image.
/**
 * On failure it drops the payload ([DSC-36]).
 *
 * \param[in] table the descriptor tables to decode with.
 * \param[in] type which record of the table msg has.
 * \param[in] payload the bytes to decode.
 * \param[in] len how many bytes payload holds.
 * \param[in] narrow nonzero when the payload came from a narrow topic.
 * \param[out] msg where to write the decoded message.
 * \param[out] field where to name the offending entry, or 0xFF when no rule
 * names one ([ERR-4]).
 * \return R2S_OK, R2S_ERR_BOUNDS for a bound the message exceeds ([ENC-16]),
 * or R2S_ERR_DECODE for a payload that breaks an encoding rule.
 */
int8_t r2s_codec_decode(
  const r2s_codec_table_t *table, uint16_t type, const uint8_t *payload, uint16_t len,
  uint8_t narrow, void *msg, uint8_t *field);

/// Dispatches a user-topic frame received while connected.
/**
 * Counts the gap ([FRM-15]), decodes subscribed topics into their receive
 * buffer and calls the topic's handler, and refuses what it cannot dispatch.
 * Defined by the topic configuration: the handwritten table for C9,
 * r2s_generated.c for G8. A build without a topic configuration never calls
 * it (R2S_WITH_CODEC).
 *
 * \param[in] topic_id the user topic the frame arrived on.
 * \param[in] seq the frame's sequence number.
 * \param[in] payload the frame's payload bytes.
 * \param[in] len how many bytes payload holds.
 */
void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
