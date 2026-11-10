// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* params: the MCU side of the parameters channel. The table below is the
 * firmware's live values; the session calls in for channel 3 frames. */
#include "r2s/r2s_params.h"

#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"

#ifdef R2S_WITH_PARAMS
static struct
{
  const r2s_param_t *table;
  uint8_t count;
} params;

/* The varint length of value, so declare sizes stay exact. */
static uint8_t varint_len(uint16_t value)
{
  uint8_t out[R2S_VARINT_MAX_BYTES];

  return r2s_varint_encode(value, out);
}

/* The bytes the default travels as (PAR-1, PAR-3): the scalar width, or the
 * length varint and the bytes for a string. */
static uint16_t default_len(const r2s_param_t *param)
{
  switch (param->type) {
    case R2S_PARAM_BOOL:
      return 1u;
    case R2S_PARAM_I64:
    case R2S_PARAM_F64:
      return 8u;
    default:
      return (uint16_t)((uint32_t)varint_len((uint16_t)strlen(param->initial.s)) +
             (uint32_t)strlen(param->initial.s));
  }
}

/* The DECLARE frame's payload length: opcode, index, type, flags, the length
 * varint and name, and the default. */
static uint32_t declare_len(const r2s_param_t *param)
{
  size_t name_len = strlen(param->name);

  return (uint32_t)4u + (uint32_t)varint_len((uint16_t)name_len) + (uint32_t)name_len +
         (uint32_t)default_len(param);
}

/* Whether the entry can be announced and answered: a name, a known type,
 * clean flags, a default the type allows, and a DECLARE frame that fits. A
 * string default longer than its bound would answer what no SET may match,
 * so the table is refused instead of announcing a lie. */
static r2s_status_t check_entry(const r2s_param_t *param)
{
  size_t len;

  if (param->name == NULL || strlen(param->name) > R2S_VARINT_MAX) {
    return R2S_ERR_BOUNDS;
  }
  if (param->type > R2S_PARAM_STRING) {
    return R2S_ERR_BOUNDS;
  }
  if ((param->flags & (uint8_t) ~R2S_PARAM_READONLY) != 0u) {
    return R2S_ERR_BOUNDS;
  }
  if (param->type == R2S_PARAM_BOOL) {
    if (param->initial.b > 1u) {
      return R2S_ERR_BOUNDS;
    }
  } else if (param->type == R2S_PARAM_STRING) {
    if (param->initial.s == NULL || param->value.s == NULL) {
      return R2S_ERR_BOUNDS;
    }
    len = strlen(param->initial.s);
    if (len > param->bound) {
      return R2S_ERR_BOUNDS;
    }
  }
    /* 17 fixed bytes, the name length varint and the name: exact, so names
     * of 128 bytes or more, whose varint takes two bytes, fit exactly. The
     * sums run in 32 bits, because on a 16-bit size_t a name near 64 KB
     * would wrap a size_t sum back under the limit. */
  if (declare_len(param) > (uint32_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD)) {
    return R2S_ERR_BOUNDS;
  }
  return R2S_OK;
}

r2s_status_t r2s_params_configure(const r2s_param_t *params_table, uint8_t count)
{
  uint8_t i;

  if (count > 0u && params_table == NULL) {
    return R2S_ERR_BOUNDS;
  }
  for (i = 0u; i < count; i++) {
    if (check_entry(&params_table[i]) != R2S_OK) {
      return R2S_ERR_BOUNDS;
    }
  }
    /* The live values start as the defaults, so a reconfiguration and a
     * reboot answer the same DECLARE. */
  for (i = 0u; i < count; i++) {
    const r2s_param_t *from = &params_table[i];
    r2s_param_t *to = (r2s_param_t *)&params_table[i];

    if (from->type == R2S_PARAM_BOOL) {
      to->value.b = from->initial.b;
    } else if (from->type == R2S_PARAM_I64) {
      to->value.i = from->initial.i;
    } else if (from->type == R2S_PARAM_F64) {
      to->value.f = from->initial.f;
    } else {
      size_t len = strlen(from->initial.s);

      memcpy(to->value.s, from->initial.s, len);
      to->value.s[len] = '\0';
    }
  }
  params.table = params_table;
  params.count = count;
  return R2S_OK;
}

uint8_t r2s_params_count(void)
{
  return params.count;
}

const r2s_param_t * r2s_params_get(uint8_t index)
{
  if (index >= params.count) {
    return NULL;
  }
  return &params.table[index];
}

/* The length of the string value: the varint must parse, name exactly its
 * bytes, and carry no NUL, or the value breaks its type's rule (PAR-3). */
static int string_len(const uint8_t *value, uint16_t len, uint16_t *out)
{
  uint16_t text_len;
  uint8_t used;
  uint16_t i;

  if (r2s_varint_decode(value, len, &text_len, &used) != R2S_VARINT_OK) {
    return 0;
  }
  if ((uint32_t)used + (uint32_t)text_len != (uint32_t)len) {
    return 0;
  }
  for (i = 0u; i < text_len; i++) {
    if (value[(uint16_t)(used + i)] == 0u) {
      return 0;
    }
  }
  *out = text_len;
  return 1;
}

int r2s_params_check_set(uint8_t index, const uint8_t *value, uint16_t len)
{
  const r2s_param_t *param;
  uint16_t text_len;

  if (index >= params.count) {
    return R2S_PARAM_UNKNOWN_INDEX;
  }
  param = &params.table[index];
  if (param->type == R2S_PARAM_BOOL) {
    if (len != 1u) {
      return R2S_PARAM_BAD_TYPE_OR_LENGTH;
    }
    if (value[0] > 1u) {
      return -1;
    }
  } else if (param->type == R2S_PARAM_I64 || param->type == R2S_PARAM_F64) {
    if (len != 8u) {
      return R2S_PARAM_BAD_TYPE_OR_LENGTH;
    }
  } else {
        /* A string value the encoding cannot carry is refused (PAR-3). */
    if (!string_len(value, len, &text_len)) {
      return -1;
    }
    if ((param->flags & R2S_PARAM_READONLY) != 0u) {
      return R2S_PARAM_READONLY_STATUS;
    }
        /* One past the bound is out of range (PAR-5). */
    if (text_len > param->bound) {
      return R2S_PARAM_OUT_OF_RANGE;
    }
  }
  if ((param->flags & R2S_PARAM_READONLY) != 0u) {
    return R2S_PARAM_READONLY_STATUS;
  }
  return R2S_PARAM_APPLIED;
}

/* The 8 wire bytes as a native 64-bit value: values travel little-endian
 * while the struct holds the target's order ([PAR-3]). */
static uint64_t wire_u64(const uint8_t *value)
{
  uint64_t bits = 0u;
  uint16_t i;

  for (i = 0u; i < 8u; i++) {
    bits |= (uint64_t)value[i] << (i * 8u);
  }
  return bits;
}

void r2s_params_store(uint8_t index, const uint8_t *value, uint16_t len)
{
    /* The caller checked the value above, so the casts cannot truncate and
     * the copy cannot overflow. Doubles move with memcpy, never with float
     * arithmetic, which the zone check forbids. */
  r2s_param_t *param = (r2s_param_t *)&params.table[index];

  if (param->type == R2S_PARAM_BOOL) {
    param->value.b = value[0];
  } else if (param->type == R2S_PARAM_I64) {
    uint64_t bits = wire_u64(value);

    memcpy(&param->value.i, &bits, 8u);
  } else if (param->type == R2S_PARAM_F64) {
    uint64_t bits = wire_u64(value);

    memcpy(&param->value.f, &bits, 8u);
  } else {
    uint16_t text_len = 0u;

    (void)string_len(value, len, &text_len);
    memcpy(param->value.s, value + (len - text_len), text_len);
    param->value.s[text_len] = '\0';
  }
}

/* The DECLARE payload length for the entry at index, or 0 when the entry is
 * missing. The session reserves the frame from this before streaming it, so
 * no buffer holds a frame. */
uint16_t r2s_params_declare_len(uint8_t index)
{
  if (index >= params.count) {
    return 0u;
  }
  return (uint16_t)declare_len(&params.table[index]);
}
#endif

#ifdef R2S_TESTING
void r2s_params_test_reset(void)
{
#ifdef R2S_WITH_PARAMS
  params.table = NULL;
  params.count = 0u;
#endif
}
#endif

#if !defined(R2S_WITH_PARAMS) && !defined(R2S_TESTING)
/* ISO C needs at least one declaration in a translation unit. */
typedef int r2s_params_unused;
#endif
