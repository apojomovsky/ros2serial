/* codec: the descriptor interpreter over flash tables (spec/protocol.md,
 * section 9). One iterative walk encodes and decodes any type: each message
 * level owns a frame on an explicit stack of R2S_MAX_NEST frames, and arrays
 * loop inside their level without growing it ([DSC-38], [DSC-40]). */
#include "r2s/r2s_codec.h"

#include <float.h>
#include <stddef.h>
#include <string.h>

#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"

/* Byte counts of one element, by kind (section 9.2). A float64 is the build's
 * double, 4 bytes where a double is 32 bits. */
static uint16_t elem_bytes(uint8_t kind)
{
    switch (kind) {
    case R2S_CODEC_BOOL:
    case R2S_CODEC_U8:
    case R2S_CODEC_I8:
        return 1u;
    case R2S_CODEC_U16:
    case R2S_CODEC_I16:
        return 2u;
    case R2S_CODEC_U32:
    case R2S_CODEC_I32:
    case R2S_CODEC_F32:
        return 4u;
    case R2S_CODEC_U64:
    case R2S_CODEC_I64:
        return 8u;
    case R2S_CODEC_F64:
        return (uint16_t)sizeof(double);
    default:
        return 0u;
    }
}

/* A multi-byte integer of a table, which keeps the wire's byte order on every
 * target ([DSC-4]). */
static uint16_t table_u16(const uint8_t *at)
{
    return (uint16_t)((uint16_t)R2S_FLASH_READ_BYTE(at) | ((uint16_t)R2S_FLASH_READ_BYTE(at + 1u) << 8u));
}

static void read_record(const r2s_codec_table_t *table, uint16_t type, uint16_t *size, uint16_t *first,
                        uint8_t *count)
{
    const uint8_t *at = table->types + (uint16_t)(5u * type);

    *size = table_u16(at);
    *first = table_u16(at + 2u);
    *count = R2S_FLASH_READ_BYTE(at + 4u);
}

static void read_entry(const r2s_codec_table_t *table, uint16_t entry, uint8_t *kind, uint8_t *mode,
                       uint16_t *offset, uint16_t *count, uint16_t *aux)
{
    const uint8_t *at = table->fields + (uint16_t)(8u * entry);

    *kind = R2S_FLASH_READ_BYTE(at);
    *mode = R2S_FLASH_READ_BYTE(at + 1u);
    *offset = table_u16(at + 2u);
    *count = table_u16(at + 4u);
    *aux = table_u16(at + 6u);
}

/* Whether values in memory are little-endian, so the interpreter knows when
 * an 8-byte field needs its bytes swapped ([DSC-34]). */
static uint8_t is_little_endian(void)
{
    const uint16_t one = 1u;

    return *(const uint8_t *)&one;
}

/* The alignment a 2-byte count member takes after bytes: the distance from a
 * leading byte to the uint16_t after it ([DSC-20]). The compiler folds the
 * distance to a constant, and both members are read below. */
struct r2s_codec_count_probe {
    uint8_t first;
    uint16_t second;
};

static uint16_t count_offset(uint32_t elem_end, uint8_t wide)
{
    /* All sums run in 32 bits, so a large table cannot wrap them ([DSC-21]). */
    struct r2s_codec_count_probe probe;
    uint32_t align = (uint32_t)(uintptr_t)&probe.second - (uint32_t)(uintptr_t)&probe.first;

    if (wide == 0u) {
        return (uint16_t)elem_end;
    }
    return (uint16_t)(elem_end + (align - (elem_end % align)) % align);
}

/* Narrowing a binary64 to binary32 with roundTiesToEven ([ENC-11]), from its
 * two halves to the 4 wire bytes. Only integer shifts and compares: the core
 * never computes with floats. Only on builds whose double is 64 bits; where
 * it is 32 bits, narrowing is a copy ([DSC-27]). */
#if DBL_MANT_DIG != FLT_MANT_DIG
static uint32_t narrow_f64(uint32_t hi, uint32_t lo)
{
    uint32_t sign = hi >> 31u;
    uint32_t exp = (hi >> 20u) & 0x7FFu;
    uint32_t mhi = hi & 0xFFFFFu;

    if (exp == 0x7FFu) {
        if ((mhi | lo) == 0u) {
            return (sign << 31u) | 0x7F800000u;
        }
        return (sign << 31u) | 0x7FC00000u;
    }
    if (exp == 0u) {
        /* A binary64 denormal is far below half the smallest binary32
         * subnormal, so it rounds to a zero of the same sign. */
        return sign << 31u;
    }
    if (exp >= 0x47Fu) {
        return (sign << 31u) | 0x7F800000u;
    }
    if (exp >= 0x381u) {
        uint32_t kept = (mhi << 3u) | (lo >> 29u);
        uint32_t rest = lo & 0x1FFFFFFFu;
        uint32_t out_exp = exp - 896u;

        if (rest > 0x10000000u || (rest == 0x10000000u && (kept & 1u) != 0u)) {
            kept++;
            if (kept == 0x800000u) {
                kept = 0u;
                out_exp++;
            }
        }
        if (out_exp >= 0xFFu) {
            return (sign << 31u) | 0x7F800000u;
        }
        return (sign << 31u) | (out_exp << 23u) | kept;
    }
    {
        /* A result below the smallest normal: round the 53-bit significand,
         * implicit 1 included, to a subnormal count. This block sees biased
         * exponents 1 to 896 only, so shift runs 30 to 925 and never 0. */
        uint32_t fhi = mhi | 0x100000u;
        uint32_t shift = 926u - exp;
        uint32_t count;
        uint32_t guard;
        uint32_t sticky;

        if (shift > 52u) {
            return sign << 31u;
        }
        if (shift < 32u) {
            count = (lo >> shift) | (fhi << (32u - shift));
            guard = (lo >> (shift - 1u)) & 1u;
            sticky = lo & ((1u << (shift - 1u)) - 1u);
        } else if (shift == 32u) {
            /* Shifting a 32-bit value by 32 is undefined, so this case
             * drops the whole low half into the guard and sticky bits. */
            count = fhi;
            guard = (lo >> 31u) & 1u;
            sticky = lo & 0x7FFFFFFFu;
        } else {
            count = fhi >> (shift - 32u);
            guard = (fhi >> (shift - 33u)) & 1u;
            sticky = (fhi & ((1u << (shift - 33u)) - 1u)) | (lo == 0u ? 0u : 1u);
        }
        if (guard != 0u && (sticky != 0u || (count & 1u) != 0u)) {
            count++;
        }
        if (count == 0x800000u) {
            return (sign << 31u) | 0x00800000u;
        }
        return (sign << 31u) | count;
    }
}

/* Widening a binary32 to binary64, which is exact, into 8 little-endian
 * bytes. A binary32 denormal normalizes into the binary64, since the wider
 * type holds it exactly. */
static void widen_f32(uint32_t bits, uint8_t out[8])
{
    uint32_t sign = bits >> 31u;
    uint32_t exp = (bits >> 23u) & 0xFFu;
    uint32_t mant = bits & 0x7FFFFFu;
    uint32_t hi;
    uint32_t lo;

    if (exp == 0xFFu) {
        hi = (sign << 31u) | (0x7FFu << 20u) | (mant >> 3u);
        lo = (mant & 7u) << 29u;
    } else if (exp == 0u) {
        if (mant == 0u) {
            hi = sign << 31u;
            lo = 0u;
        } else {
            uint32_t places = 0u;

            while ((mant & 0x800000u) == 0u) {
                mant <<= 1u;
                places++;
            }
            mant &= 0x7FFFFFu;
            hi = (sign << 31u) | ((897u - places) << 20u) | (mant >> 3u);
            lo = (mant & 7u) << 29u;
        }
    } else {
        hi = (sign << 31u) | ((exp + 896u) << 20u) | (mant >> 3u);
        lo = (mant & 7u) << 29u;
    }
    out[0] = (uint8_t)(lo & 0xFFu);
    out[1] = (uint8_t)((lo >> 8u) & 0xFFu);
    out[2] = (uint8_t)((lo >> 16u) & 0xFFu);
    out[3] = (uint8_t)((lo >> 24u) & 0xFFu);
    out[4] = (uint8_t)(hi & 0xFFu);
    out[5] = (uint8_t)((hi >> 8u) & 0xFFu);
    out[6] = (uint8_t)((hi >> 16u) & 0xFFu);
    out[7] = (uint8_t)((hi >> 24u) & 0xFFu);
}
#endif

/* One message level on the explicit nesting stack. next and end are global
 * entry indices; left and step walk an array of nested messages without
 * growing the stack ([DSC-38]). */
typedef struct {
    uint16_t type;
    uint16_t next;
    uint16_t end;
    uint16_t left;
    uint16_t step;
    const uint8_t *base;
} r2s_enc_frame_t;

typedef struct {
    uint16_t type;
    uint16_t next;
    uint16_t end;
    uint16_t left;
    uint16_t step;
    uint8_t *base;
} r2s_dec_frame_t;

/* Emits one byte to the sink, which never truncates ([DSC-35]). */
static int8_t emit_byte(r2s_codec_out_t *out, uint8_t byte)
{
    if (out->to_tx != 0u) {
        (void)r2s_tx_byte(byte);
        out->len++;
        return R2S_OK;
    }
    if (out->buf == NULL) {
        out->len++;
        return R2S_OK;
    }
    if (out->len >= out->cap) {
        return R2S_ERR_BOUNDS;
    }
    out->buf[out->len] = byte;
    out->len++;
    return R2S_OK;
}

/* Emits up to 4 bytes of a scalar, least significant first ([GEN-1]). */
static int8_t emit_copy(r2s_codec_out_t *out, const uint8_t *at, uint16_t width, uint8_t le)
{
    uint16_t i;

    for (i = 0u; i < width; i++) {
        uint8_t byte = (le != 0u) ? at[i] : at[(uint16_t)(width - 1u - i)];

        if (emit_byte(out, byte) != R2S_OK) {
            return R2S_ERR_BOUNDS;
        }
    }
    return R2S_OK;
}

/* Emits the 8 bytes of a 64-bit field, least significant first. */
static int8_t emit_wide(r2s_codec_out_t *out, const uint8_t *at, uint8_t le)
{
    uint8_t tmp[8];
    uint16_t i;

    for (i = 0u; i < 8u; i++) {
        tmp[i] = (le != 0u) ? at[i] : at[7u - i];
    }
    for (i = 0u; i < 8u; i++) {
        if (emit_byte(out, tmp[i]) != R2S_OK) {
            return R2S_ERR_BOUNDS;
        }
    }
    return R2S_OK;
}

static int8_t emit_varint(r2s_codec_out_t *out, uint16_t value)
{
    uint8_t encoded[R2S_VARINT_MAX_BYTES];
    uint8_t n = r2s_varint_encode(value, encoded);
    uint8_t i;

    for (i = 0u; i < n; i++) {
        if (emit_byte(out, encoded[i]) != R2S_OK) {
            return R2S_ERR_BOUNDS;
        }
    }
    return R2S_OK;
}

/* Pushes the nested type for one element, refusing when the stack is full. */
static int8_t push_nest(r2s_enc_frame_t *stack, uint16_t *sp, const r2s_codec_table_t *table, uint16_t nested,
                        const uint8_t *at, uint16_t left, uint16_t step, uint8_t *field, uint16_t visited)
{
    uint16_t size;
    uint16_t first;
    uint8_t count;

    if ((uint16_t)(*sp + 1u) >= (uint16_t)R2S_MAX_NEST) {
        *field = (uint8_t)(visited - 1u);
        return R2S_ERR_BOUNDS;
    }
    read_record(table, nested, &size, &first, &count);
    (void)size;
    (*sp)++;
    stack[*sp].type = nested;
    stack[*sp].next = first;
    stack[*sp].end = (uint16_t)(first + count);
    stack[*sp].left = left;
    stack[*sp].step = step;
    stack[*sp].base = at;
    return R2S_OK;
}

/* Drains one level: on exhaustion either repeats the next array element in
 * the same slot or pops. Returns nonzero when the whole message is done. */
static uint8_t drain_enc(r2s_enc_frame_t *stack, uint16_t *sp, const r2s_codec_table_t *table)
{
    if (stack[*sp].left > 0u) {
        uint16_t size;
        uint16_t first;
        uint8_t count;

        read_record(table, stack[*sp].type, &size, &first, &count);
        (void)size;
        (void)count;
        stack[*sp].base += stack[*sp].step;
        stack[*sp].next = first;
        stack[*sp].left--;
        return 0u;
    }
    if (*sp == 0u) {
        return 1u;
    }
    (*sp)--;
    return 0u;
}

int8_t r2s_codec_encode(const r2s_codec_table_t *table, uint16_t type, const void *msg, uint8_t narrow,
                        r2s_codec_out_t *out, uint8_t *field)
{
    r2s_enc_frame_t stack[R2S_MAX_NEST];
    uint16_t visited = 0u;
    uint8_t le = is_little_endian();
    uint16_t sp = 0u;

    {
        uint16_t size;
        uint8_t count;

        read_record(table, type, &size, &stack[0].next, &count);
        (void)size;
        stack[0].type = type;
        stack[0].end = (uint16_t)(stack[0].next + count);
        stack[0].left = 0u;
        stack[0].step = 0u;
        stack[0].base = (const uint8_t *)msg;
    }
    out->len = 0u;
    for (;;) {
        uint8_t kind;
        uint8_t mode;
        uint16_t offset;
        uint16_t count;
        uint16_t aux;
        const uint8_t *at;
        uint16_t width;
        uint16_t elems;
        uint16_t i;

        if (stack[sp].next == stack[sp].end) {
            if (drain_enc(stack, &sp, table) != 0u) {
                return R2S_OK;
            }
            continue;
        }
        read_entry(table, stack[sp].next, &kind, &mode, &offset, &count, &aux);
        visited++;
        at = stack[sp].base + offset;
        if (kind == R2S_CODEC_NEST) {
            uint16_t nested_size;
            uint16_t nested_first;
            uint8_t nested_count;
            uint16_t have;

            read_record(table, aux, &nested_size, &nested_first, &nested_count);
            (void)nested_first;
            (void)nested_count;
            if (mode == R2S_CODEC_SCALAR) {
                have = 1u;
            } else if (mode == R2S_CODEC_FIXED) {
                have = count;
            } else {
                uint32_t elems_end = (uint32_t)count * nested_size;
                uint16_t member = count_offset(elems_end, (uint8_t)((mode & R2S_CODEC_COUNT16) != 0u));
                uint16_t stored;

                if ((mode & R2S_CODEC_COUNT16) != 0u) {
                    memcpy(&stored, at + member, 2u);
                } else {
                    stored = at[member];
                }
                if (stored > count) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_BOUNDS;
                }
                if (emit_varint(out, stored) != R2S_OK) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_BOUNDS;
                }
                have = stored;
            }
            stack[sp].next++;
            if (have == 0u) {
                continue;
            }
            if (push_nest(stack, &sp, table, aux, at, (uint16_t)(have - 1u), nested_size, field, visited) !=
                R2S_OK) {
                return R2S_ERR_BOUNDS;
            }
            continue;
        }
        if (kind == R2S_CODEC_STR) {
            uint32_t n = 0u;

            stack[sp].next++;
            while (n <= aux && at[n] != 0u) {
                n++;
            }
            if (n > aux) {
                *field = (uint8_t)(visited - 1u);
                return R2S_ERR_BOUNDS;
            }
            if (emit_varint(out, (uint16_t)n) != R2S_OK) {
                *field = (uint8_t)(visited - 1u);
                return R2S_ERR_BOUNDS;
            }
            for (uint32_t k = 0u; k < n; k++) {
                if (emit_byte(out, at[k]) != R2S_OK) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_BOUNDS;
                }
            }
            continue;
        }
        width = elem_bytes(kind);
        if (mode == R2S_CODEC_SCALAR) {
            elems = 1u;
        } else if (mode == R2S_CODEC_FIXED) {
            elems = count;
        } else {
            uint32_t elems_end = (uint32_t)count * width;
            uint16_t member = count_offset(elems_end, (uint8_t)((mode & R2S_CODEC_COUNT16) != 0u));
            uint16_t stored;

            if ((mode & R2S_CODEC_COUNT16) != 0u) {
                memcpy(&stored, at + member, 2u);
            } else {
                stored = at[member];
            }
            if (stored > count) {
                *field = (uint8_t)(visited - 1u);
                return R2S_ERR_BOUNDS;
            }
            elems = stored;
            if (emit_varint(out, stored) != R2S_OK) {
                *field = (uint8_t)(visited - 1u);
                return R2S_ERR_BOUNDS;
            }
        }
        stack[sp].next++;
        for (i = 0u; i < elems; i++) {
            const uint8_t *item = at + (uint16_t)(i * width);
            int8_t rc;

            if (kind == R2S_CODEC_BOOL) {
                if (*item > 1u) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_BOUNDS;
                }
                rc = emit_byte(out, *item);
            } else if (kind == R2S_CODEC_F64 && narrow != 0u) {
#if DBL_MANT_DIG == FLT_MANT_DIG
                rc = emit_copy(out, item, 4u, le);
#else
                uint8_t ordered[8];
                uint32_t hi;
                uint32_t lo;
                uint32_t bits;
                uint16_t k;

                for (k = 0u; k < 8u; k++) {
                    ordered[k] = (le != 0u) ? item[k] : item[7u - k];
                }
                lo = (uint32_t)ordered[0] | ((uint32_t)ordered[1] << 8u) | ((uint32_t)ordered[2] << 16u) |
                     ((uint32_t)ordered[3] << 24u);
                hi = (uint32_t)ordered[4] | ((uint32_t)ordered[5] << 8u) | ((uint32_t)ordered[6] << 16u) |
                     ((uint32_t)ordered[7] << 24u);
                bits = narrow_f64(hi, lo);
                rc = R2S_OK;
                for (k = 0u; k < 4u; k++) {
                    if (emit_byte(out, (uint8_t)((bits >> (8u * k)) & 0xFFu)) != R2S_OK) {
                        rc = R2S_ERR_BOUNDS;
                        break;
                    }
                }
#endif
            } else if (width == 8u) {
                rc = emit_wide(out, item, le);
            } else {
                rc = emit_copy(out, item, width, le);
            }
            if (rc != R2S_OK) {
                *field = (uint8_t)(visited - 1u);
                return rc;
            }
        }
    }
}

/* Takes one byte or reports the truncated payload ([ENC-17]). */
static int8_t take_byte(const uint8_t **p, const uint8_t *end, uint8_t *byte)
{
    if (*p == end) {
        return R2S_ERR_DECODE;
    }
    *byte = **p;
    (*p)++;
    return R2S_OK;
}

/* Takes up to 4 bytes into their struct order. */
static int8_t take_copy(const uint8_t **p, const uint8_t *end, uint8_t *at, uint16_t width, uint8_t le)
{
    uint16_t i;

    for (i = 0u; i < width; i++) {
        uint8_t byte;

        if (take_byte(p, end, &byte) != R2S_OK) {
            return R2S_ERR_DECODE;
        }
        at[(le != 0u) ? i : (uint16_t)(width - 1u - i)] = byte;
    }
    return R2S_OK;
}

/* Takes the 8 bytes of a 64-bit field into their struct order. */
static int8_t take_wide(const uint8_t **p, const uint8_t *end, uint8_t *at, uint8_t le)
{
    uint8_t tmp[8];
    uint16_t i;

    for (i = 0u; i < 8u; i++) {
        if (take_byte(p, end, &tmp[i]) != R2S_OK) {
            return R2S_ERR_DECODE;
        }
    }
    for (i = 0u; i < 8u; i++) {
        at[i] = (le != 0u) ? tmp[i] : tmp[7u - i];
    }
    return R2S_OK;
}

/* Takes a varint length, whose own rules reject it ([VAR-2] to [VAR-4]). */
static int8_t take_varint(const uint8_t **p, const uint8_t *end, uint16_t *value)
{
    uint8_t used = 0u;

    if (r2s_varint_decode(*p, (uint16_t)(end - *p), value, &used) != R2S_VARINT_OK) {
        return R2S_ERR_DECODE;
    }
    *p += used;
    return R2S_OK;
}

static int8_t push_nest_dec(r2s_dec_frame_t *stack, uint16_t *sp, const r2s_codec_table_t *table,
                            uint16_t nested, uint8_t *at, uint16_t left, uint16_t step, uint8_t *field,
                            uint16_t visited)
{
    uint16_t size;
    uint16_t first;
    uint8_t count;

    if ((uint16_t)(*sp + 1u) >= (uint16_t)R2S_MAX_NEST) {
        *field = (uint8_t)(visited - 1u);
        return R2S_ERR_BOUNDS;
    }
    read_record(table, nested, &size, &first, &count);
    (void)size;
    (*sp)++;
    stack[*sp].type = nested;
    stack[*sp].next = first;
    stack[*sp].end = (uint16_t)(first + count);
    stack[*sp].left = left;
    stack[*sp].step = step;
    stack[*sp].base = at;
    return R2S_OK;
}

static uint8_t drain_dec(r2s_dec_frame_t *stack, uint16_t *sp, const r2s_codec_table_t *table)
{
    if (stack[*sp].left > 0u) {
        uint16_t size;
        uint16_t first;
        uint8_t count;

        read_record(table, stack[*sp].type, &size, &first, &count);
        (void)size;
        (void)count;
        stack[*sp].base += stack[*sp].step;
        stack[*sp].next = first;
        stack[*sp].left--;
        return 0u;
    }
    if (*sp == 0u) {
        return 1u;
    }
    (*sp)--;
    return 0u;
}

int8_t r2s_codec_decode(const r2s_codec_table_t *table, uint16_t type, const uint8_t *payload, uint16_t len,
                        uint8_t narrow, void *msg, uint8_t *field)
{
    r2s_dec_frame_t stack[R2S_MAX_NEST];
    const uint8_t *p = payload;
    const uint8_t *end = payload + len;
    uint16_t visited = 0u;
    uint8_t le = is_little_endian();
    uint16_t sp = 0u;

    {
        uint16_t size;
        uint8_t count;

        read_record(table, type, &size, &stack[0].next, &count);
        (void)size;
        stack[0].type = type;
        stack[0].end = (uint16_t)(stack[0].next + count);
        stack[0].left = 0u;
        stack[0].step = 0u;
        stack[0].base = (uint8_t *)msg;
    }
    for (;;) {
        uint8_t kind;
        uint8_t mode;
        uint16_t offset;
        uint16_t count;
        uint16_t aux;
        uint8_t *at;
        uint16_t width;
        uint16_t elems;
        uint16_t i;

        if (stack[sp].next == stack[sp].end) {
            if (drain_dec(stack, &sp, table) != 0u) {
                if (p != end) {
                    *field = 0xFFu;
                    return R2S_ERR_DECODE;
                }
                return R2S_OK;
            }
            continue;
        }
        read_entry(table, stack[sp].next, &kind, &mode, &offset, &count, &aux);
        visited++;
        at = stack[sp].base + offset;
        if (kind == R2S_CODEC_NEST) {
            uint16_t nested_size;
            uint16_t nested_first;
            uint8_t nested_count;
            uint16_t have;

            read_record(table, aux, &nested_size, &nested_first, &nested_count);
            (void)nested_first;
            (void)nested_count;
            if (mode == R2S_CODEC_SCALAR) {
                have = 1u;
            } else if (mode == R2S_CODEC_FIXED) {
                have = count;
            } else {
                uint16_t stored;

                if (take_varint(&p, end, &stored) != R2S_OK) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_DECODE;
                }
                if (stored > count) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_BOUNDS;
                }
                if ((mode & R2S_CODEC_COUNT16) != 0u) {
                    uint16_t member = count_offset((uint32_t)count * nested_size, 1u);

                    memcpy(at + member, &stored, 2u);
                } else {
                    uint16_t member = count_offset((uint32_t)count * nested_size, 0u);

                    at[member] = (uint8_t)stored;
                }
                have = stored;
            }
            stack[sp].next++;
            if (have == 0u) {
                continue;
            }
            if (push_nest_dec(stack, &sp, table, aux, at, (uint16_t)(have - 1u), nested_size, field,
                              visited) != R2S_OK) {
                return R2S_ERR_BOUNDS;
            }
            continue;
        }
        if (kind == R2S_CODEC_STR) {
            uint16_t length;
            uint16_t k;

            stack[sp].next++;
            if (take_varint(&p, end, &length) != R2S_OK) {
                *field = (uint8_t)(visited - 1u);
                return R2S_ERR_DECODE;
            }
            if (length > aux) {
                *field = (uint8_t)(visited - 1u);
                return R2S_ERR_BOUNDS;
            }
            for (k = 0u; k < length; k++) {
                uint8_t byte;

                if (take_byte(&p, end, &byte) != R2S_OK) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_DECODE;
                }
                if (byte == 0u) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_DECODE;
                }
                at[k] = byte;
            }
            at[length] = 0u;
            continue;
        }
        width = elem_bytes(kind);
        if (mode == R2S_CODEC_SCALAR) {
            elems = 1u;
        } else if (mode == R2S_CODEC_FIXED) {
            elems = count;
        } else {
            uint16_t stored;

            if (take_varint(&p, end, &stored) != R2S_OK) {
                *field = (uint8_t)(visited - 1u);
                return R2S_ERR_DECODE;
            }
            if (stored > count) {
                *field = (uint8_t)(visited - 1u);
                return R2S_ERR_BOUNDS;
            }
            if ((mode & R2S_CODEC_COUNT16) != 0u) {
                uint16_t member = count_offset((uint32_t)count * width, 1u);

                memcpy(at + member, &stored, 2u);
            } else {
                uint16_t member = count_offset((uint32_t)count * width, 0u);

                at[member] = (uint8_t)stored;
            }
            elems = stored;
        }
        stack[sp].next++;
        for (i = 0u; i < elems; i++) {
            uint8_t *item = at + (uint16_t)(i * width);
            int8_t rc;

            if (kind == R2S_CODEC_BOOL) {
                uint8_t byte;

                if (take_byte(&p, end, &byte) != R2S_OK) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_DECODE;
                }
                if (byte > 1u) {
                    *field = (uint8_t)(visited - 1u);
                    return R2S_ERR_DECODE;
                }
                *item = byte;
                continue;
            }
            if (kind == R2S_CODEC_F64 && narrow != 0u) {
#if DBL_MANT_DIG == FLT_MANT_DIG
                rc = take_copy(&p, end, item, 4u, le);
#else
                uint32_t bits = 0u;
                uint16_t k;
                uint8_t wide[8];

                for (k = 0u; k < 4u; k++) {
                    uint8_t byte;

                    if (take_byte(&p, end, &byte) != R2S_OK) {
                        *field = (uint8_t)(visited - 1u);
                        return R2S_ERR_DECODE;
                    }
                    bits |= (uint32_t)byte << (8u * k);
                }
                widen_f32(bits, wide);
                if (le != 0u) {
                    memcpy(item, wide, 8u);
                } else {
                    for (k = 0u; k < 8u; k++) {
                        item[k] = wide[7u - k];
                    }
                }
                continue;
#endif
            } else if (width == 8u) {
                rc = take_wide(&p, end, item, le);
            } else {
                rc = take_copy(&p, end, item, width, le);
            }
            if (rc != R2S_OK) {
                *field = (uint8_t)(visited - 1u);
                return rc;
            }
        }
    }
}
