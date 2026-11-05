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

/* The parameter types [PAR-2] names, in payload order. */
#define R2S_PARAM_BOOL 0u
#define R2S_PARAM_I64 1u
#define R2S_PARAM_F64 2u
#define R2S_PARAM_STRING 3u

/* The flags byte [PAR-4] names: bit 0 is read-only, the rest are reserved. */
#define R2S_PARAM_READONLY 0x01u

/* The opcodes of the parameters channel, in payload order. */
#define R2S_PARAM_OP_REQ 0x01u
#define R2S_PARAM_OP_DECLARE 0x02u
#define R2S_PARAM_OP_SET 0x03u
#define R2S_PARAM_OP_STATUS 0x04u

/* The PARAM_STATUS verdicts [PAR-5] names, in payload order. */
#define R2S_PARAM_APPLIED 0u
#define R2S_PARAM_UNKNOWN_INDEX 1u
#define R2S_PARAM_BAD_TYPE_OR_LENGTH 2u
#define R2S_PARAM_READONLY_STATUS 3u
#define R2S_PARAM_OUT_OF_RANGE 4u

/* One parameter: its name, type and flags, its default and its live value.
 * Strings live in the caller's buffer of bound bytes plus the NUL; the
 * default points at the initial text. Scalars carry the default inline. */
typedef struct {
    const char *name;
    uint8_t type;
    uint8_t flags;
    uint16_t bound;
    union {
        uint8_t b;
        int64_t i;
        double f;
        const char *s;
    } initial;
    union {
        uint8_t b;
        int64_t i;
        double f;
        char *s;
    } value;
} r2s_param_t;

#ifdef R2S_WITH_PARAMS
/* Hands the session its parameter table: count entries at params, each with
 * a name, a known type, clean flags and a default the DECLARE frame can
 * carry. The live values start as the defaults. Returns R2S_OK, or
 * R2S_ERR_BOUNDS when the table is missing, a name or default is missing or
 * unfit, or a DECLARE frame would not fit, leaving the old table unchanged. */
r2s_status_t r2s_params_configure(const r2s_param_t *params, uint8_t count);

/* How many parameters the table holds, 0 before any configuration. */
uint8_t r2s_params_count(void);

/* The table entry at index, or NULL past the count. The caller reads the
 * live value the last applied SET left; the session writes it. */
const r2s_param_t *r2s_params_get(uint8_t index);
#endif

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_params_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
