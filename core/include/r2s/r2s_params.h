/* params: the MCU side of the parameters channel (spec/protocol.md, section
 * 7.6). The table lives in RAM with the firmware's live values; configure
 * copies each default into its live value. Compiled only with
 * R2S_WITH_PARAMS. */
#ifndef R2S_PARAMS_H
#define R2S_PARAMS_H

#include <stdint.h>

#include "r2s/r2s.h"

#ifdef __cplusplus
extern "C" {
#endif

// The parameter types [PAR-2] names, in payload order.
/// Boolean.
#define R2S_PARAM_BOOL 0u
/// 64-bit signed integer.
#define R2S_PARAM_I64 1u
/// 64-bit float.
#define R2S_PARAM_F64 2u
/// String.
#define R2S_PARAM_STRING 3u

// The flags byte [PAR-4] names: bit 0 is read-only, the rest are reserved.
/// Read-only flag: bit 0 of the flags byte.
#define R2S_PARAM_READONLY 0x01u

// The opcodes of the parameters channel, in payload order.
/// Host asks for the table.
#define R2S_PARAM_OP_REQ 0x01u
/// MCU declares one parameter.
#define R2S_PARAM_OP_DECLARE 0x02u
/// Host sets one parameter.
#define R2S_PARAM_OP_SET 0x03u
/// MCU answers a set.
#define R2S_PARAM_OP_STATUS 0x04u

// The PARAM_STATUS verdicts [PAR-5] names, in payload order.
/// The set applied.
#define R2S_PARAM_APPLIED 0u
/// No parameter has that index.
#define R2S_PARAM_UNKNOWN_INDEX 1u
/// The value breaks the type or length rule.
#define R2S_PARAM_BAD_TYPE_OR_LENGTH 2u
/// The parameter is read-only.
#define R2S_PARAM_READONLY_STATUS 3u
/// The value is outside the range.
#define R2S_PARAM_OUT_OF_RANGE 4u

/// One parameter: its name, type and flags, its default and its live value.
/**
 * Strings live in the caller's buffer of bound bytes plus the NUL; the
 * default points at the initial text. Scalars carry the default inline.
 */
typedef struct
{
  const char *name;  ///< The parameter name.
  uint8_t type;      ///< One of the parameter types above.
  uint8_t flags;     ///< The flags byte above.
  uint16_t bound;    ///< The string bound in bytes, without the NUL.
  union {
    uint8_t b;      ///< The boolean default.
    int64_t i;      ///< The integer default.
    double f;       ///< The float default.
    const char *s;  ///< The string default.
  } initial;        ///< The default the DECLARE frame carries.
  union {
    uint8_t b;  ///< The boolean live value.
    int64_t i;  ///< The integer live value.
    double f;   ///< The float live value.
    char *s;    ///< The string live value.
  } value;      ///< The live value the last applied SET left.
} r2s_param_t;

#ifdef R2S_WITH_PARAMS
/// Hands the session its parameter table.
/**
 * Installs count entries at params, each with a name, a known type, clean
 * flags and a default the DECLARE frame can carry. The live values start as
 * the defaults.
 *
 * \param[in] params the table to install.
 * \param[in] count how many entries params holds.
 * \return R2S_OK, or R2S_ERR_BOUNDS when the table is missing, a name or
 * default is missing or unfit, or a DECLARE frame would not fit, leaving the
 * old table unchanged.
 * \pre The caller keeps params alive past the call.
 */
r2s_status_t r2s_params_configure(const r2s_param_t *params, uint8_t count);

/// How many parameters the table holds, 0 before any configuration.
/**
 * \return The configured count.
 */
uint8_t r2s_params_count(void);

/// The table entry at index, or NULL past the count.
/**
 * The caller reads the live value the last applied SET left; the session
 * writes it.
 *
 * \param[in] index which entry to read.
 * \return The entry, or NULL past the count.
 */
const r2s_param_t * r2s_params_get(uint8_t index);

/// The verdict a SET value earns without applying it.
/**
 * The order is unknown index, length, read-only, then range.
 *
 * \param[in] index which entry the SET names.
 * \param[in] value the SET value bytes.
 * \param[in] len how many bytes value holds.
 * \return One of the status codes above, or -1 when the value breaks its
 * type's rule and the session refuses the frame (PAR-3, PAR-5).
 */
int r2s_params_check_set(uint8_t index, const uint8_t *value, uint16_t len);

/// Keeps the checked SET value as the live one.
/**
 * \param[in] index which entry to store to.
 * \param[in] value the checked SET value bytes.
 * \param[in] len how many bytes value holds.
 * \pre The caller checked the value with r2s_params_check_set() first, so
 * the copy cannot overflow.
 */
void r2s_params_store(uint8_t index, const uint8_t *value, uint16_t len);

/// The DECLARE payload length for the entry at index, or 0 when missing.
/**
 * The session reserves the frame from this before streaming it, so no buffer
 * holds a frame.
 *
 * \param[in] index which entry to measure.
 * \return The DECLARE payload length, or 0 when the entry is missing.
 */
uint16_t r2s_params_declare_len(uint8_t index);
#endif

#ifdef R2S_TESTING
/// Resets the parameters for host test builds.
/**
 * The same reset, as r2s_test_reset() calls it.
 */
void r2s_params_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
