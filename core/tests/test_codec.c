/* The codec interpreter over handwritten and purpose-built tables: scalars in
 * both byte orders, arrays, strings, nesting and the narrow conversion. */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "handwritten.h"
#include "r2s/r2s_codec.h"
#include "unity.h"

typedef struct {
    uint8_t flag;
    uint8_t u8;
    int8_t i8;
    uint16_t u16;
    int16_t i16;
    uint32_t u32;
    int32_t i32;
    float f32;
    uint8_t u64v[8];
    uint8_t i64v[8];
} test_scalars_t;

typedef char check_scalars_size[sizeof(test_scalars_t) == 36u ? 1 : -1];
typedef char check_scalars_u16[offsetof(test_scalars_t, u16) == 4u ? 1 : -1];
typedef char check_scalars_u32[offsetof(test_scalars_t, u32) == 8u ? 1 : -1];
typedef char check_scalars_f32[offsetof(test_scalars_t, f32) == 16u ? 1 : -1];
typedef char check_scalars_u64[offsetof(test_scalars_t, u64v) == 20u ? 1 : -1];
typedef char check_scalars_i64[offsetof(test_scalars_t, i64v) == 28u ? 1 : -1];

static const uint8_t test_scalars_types[] = {0x24u, 0x00u, 0x00u, 0x00u, 0x0Au};
static const uint8_t test_scalars_fields[] = {
    0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* bool at 0 */
    0x01u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* u8 at 1 */
    0x02u, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* i8 at 2 */
    0x03u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* u16 at 4 */
    0x04u, 0x00u, 0x06u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* i16 at 6 */
    0x05u, 0x00u, 0x08u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* u32 at 8 */
    0x06u, 0x00u, 0x0Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* i32 at 12 */
    0x09u, 0x00u, 0x10u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* f32 at 16 */
    0x07u, 0x00u, 0x14u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* u64 at 20 */
    0x08u, 0x00u, 0x1Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* i64 at 28 */
};
static const r2s_codec_table_t test_scalars_table = {test_scalars_types, 1u, test_scalars_fields, 10u};

/* A single float64 for the narrow conversion. */
typedef struct {
    double v;
} test_f64_t;

static const uint8_t test_f64_types[] = {0x08u, 0x00u, 0x00u, 0x00u, 0x01u};
static const uint8_t test_f64_fields[] = {0x0Au, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
static const r2s_codec_table_t test_f64_table = {test_f64_types, 1u, test_f64_fields, 1u};

/* A string, a fixed string array and a bounded one: each string element is
 * aux + 1 bytes, and the bounded form keeps its count past the last one. */
typedef struct {
    char name[6];
} test_str_t;

typedef struct {
    char name[3][6];
} test_fixed_str_t;

typedef struct {
    char data[4][6];
    uint8_t n;
} test_bounded_str_t;

typedef struct {
    char data[300][6];
    uint16_t n;
} test_bounded16_str_t;

typedef struct {
    char fixed[2][6];
    char data[2][6];
    uint8_t n;
} test_str_pair_t;

typedef struct {
    uint16_t v[3];
} test_fixed_t;

typedef struct {
    uint8_t v[4];
    uint8_t n;
} test_bounded_t;

typedef struct {
    uint8_t v[300];
    uint16_t n;
} test_bounded16_t;

typedef char check_str_size[sizeof(test_str_t) == 6u ? 1 : -1];
typedef char check_fixed_size[sizeof(test_fixed_t) == 6u ? 1 : -1];
typedef char check_bounded_size[sizeof(test_bounded_t) == 5u ? 1 : -1];
typedef char check_bounded16_count[offsetof(test_bounded16_t, n) == 300u ? 1 : -1];
typedef char check_fixed_str_size[sizeof(test_fixed_str_t) == 18u ? 1 : -1];
typedef char check_bounded_str_count[offsetof(test_bounded_str_t, n) == 24u ? 1 : -1];
typedef char check_bounded16_str_count[offsetof(test_bounded16_str_t, n) == 1800u ? 1 : -1];
typedef char check_str_pair_data[offsetof(test_str_pair_t, data) == 12u ? 1 : -1];
typedef char check_str_pair_count[offsetof(test_str_pair_t, n) == 24u ? 1 : -1];

static const uint8_t test_arrays_types[] = {
    0x06u, 0x00u, 0x00u, 0x00u, 0x01u, 0x06u, 0x00u, 0x01u, 0x00u, 0x01u, 0x05u, 0x00u, 0x02u, 0x00u,
    0x01u, 0x2Eu, 0x01u, 0x03u, 0x00u, 0x01u, 0x12u, 0x00u, 0x04u, 0x00u, 0x01u, 0x19u, 0x00u, 0x05u,
    0x00u, 0x01u, 0x0Au, 0x07u, 0x06u, 0x00u, 0x01u, 0x19u, 0x00u, 0x07u, 0x00u, 0x02u,
};
static const uint8_t test_arrays_fields[] = {
    0x0Bu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x05u, 0x00u, /* string<=5 at 0 */
    0x03u, 0x01u, 0x00u, 0x00u, 0x03u, 0x00u, 0x00u, 0x00u, /* u16[3] at 0 */
    0x01u, 0x02u, 0x00u, 0x00u, 0x04u, 0x00u, 0x00u, 0x00u, /* u8[<=4] at 0 */
    0x01u, 0x82u, 0x00u, 0x00u, 0x2Cu, 0x01u, 0x00u, 0x00u, /* u8[<=300] at 0 */
    0x0Bu, 0x01u, 0x00u, 0x00u, 0x03u, 0x00u, 0x05u, 0x00u, /* string<=5[3] at 0 */
    0x0Bu, 0x02u, 0x00u, 0x00u, 0x04u, 0x00u, 0x05u, 0x00u, /* string<=5[<=4] at 0 */
    0x0Bu, 0x82u, 0x00u, 0x00u, 0x2Cu, 0x01u, 0x05u, 0x00u, /* string<=5[<=300] at 0 */
    0x0Bu, 0x01u, 0x00u, 0x00u, 0x02u, 0x00u, 0x05u, 0x00u, /* string<=5[2] at 0 */
    0x0Bu, 0x02u, 0x0Cu, 0x00u, 0x02u, 0x00u, 0x05u, 0x00u, /* string<=5[<=2] at 12 */
};
static const r2s_codec_table_t test_arrays_table = {test_arrays_types, 8u, test_arrays_fields, 9u};

/* Three nesting levels: inner, middle and an outer too deep for the stack. */
typedef struct {
    uint8_t tag;
} test_inner_t;

typedef struct {
    test_inner_t in;
    uint8_t after;
} test_mid_t;

typedef struct {
    test_mid_t m1;
    test_mid_t m2;
} test_outer_t;

static const uint8_t test_nest_types[] = {
    0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 0x00u, 0x01u, 0x00u, 0x02u,
    0x04u, 0x00u, 0x03u, 0x00u, 0x02u, 0x34u, 0x01u, 0x05u, 0x00u, 0x03u,
};
static const uint8_t test_nest_fields[] = {
    0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* tag at 0 */
    0x0Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* inner at 0 */
    0x01u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, /* after at 1 */
    0x0Cu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, /* m1 at 0 */
    0x0Cu, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, /* m2 at 2 */
    0x0Cu, 0x01u, 0x00u, 0x00u, 0x02u, 0x00u, 0x00u, 0x00u, /* fx[2] at 0 */
    0x0Cu, 0x82u, 0x02u, 0x00u, 0x2Cu, 0x01u, 0x00u, 0x00u, /* b16[<=300] at 2 */
    0x0Cu, 0x02u, 0x30u, 0x01u, 0x02u, 0x00u, 0x00u, 0x00u, /* bd[<=2] at 304 */
};
static const r2s_codec_table_t test_nest_table = {test_nest_types, 4u, test_nest_fields, 8u};

/* A holder of nested arrays: a fixed pair, a bounded run of up to 300 and a
 * bounded pair, all over the depth-1 inner type so the holder itself stays
 * within the nesting stack. */
typedef struct {
    test_inner_t fx[2];
    test_inner_t b16[300];
    uint16_t b16_n;
    test_inner_t bd[2];
    uint8_t bd_n;
} test_holder_t;

typedef char check_holder_size[sizeof(test_holder_t) == 308u ? 1 : -1];
typedef char check_holder_b16[offsetof(test_holder_t, b16) == 2u ? 1 : -1];
typedef char check_holder_b16_n[offsetof(test_holder_t, b16_n) == 302u ? 1 : -1];
typedef char check_holder_bd[offsetof(test_holder_t, bd) == 304u ? 1 : -1];
typedef char check_holder_bd_n[offsetof(test_holder_t, bd_n) == 306u ? 1 : -1];

static uint16_t encode_here(const r2s_codec_table_t *table, uint16_t type, const void *msg, uint8_t narrow,
                            uint8_t *buf, uint16_t cap, int8_t *rc, uint8_t *field)
{
    r2s_codec_out_t sink = {buf, cap, 0u, 0u};

    *field = 0xFFu;
    *rc = r2s_codec_encode(table, type, msg, narrow, &sink, field);
    return sink.len;
}

static int8_t decode_here(const r2s_codec_table_t *table, uint16_t type, const uint8_t *payload, uint16_t len,
                          uint8_t narrow, void *msg, uint8_t *field)
{
    *field = 0xFFu;
    return r2s_codec_decode(table, type, payload, len, narrow, msg, field);
}

/* covers: DSC-28, DSC-29 */
void test_codec_scalars_round_trip(void)
{
    static const test_scalars_t in = {1u,
                                      0xABu,
                                      -5,
                                      0x1234u,
                                      -2,
                                      0x89ABCDEFu,
                                      -0x12345678,
                                      -2.0f,
                                      {0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u},
                                      {0xFFu, 0xEEu, 0xDDu, 0xCCu, 0xBBu, 0xAAu, 0x99u, 0x88u}};
    static uint8_t buf[64];
    test_scalars_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len = encode_here(&test_scalars_table, 0u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);

    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(35, len);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_scalars_table, 0u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_EQUAL_UINT8(0xABu, out.u8);
    TEST_ASSERT_EQUAL_INT8(-5, out.i8);
    TEST_ASSERT_EQUAL_INT32(-0x12345678, out.i32);
}

/* covers: DSC-29, DSC-34 */
void test_codec_multibyte_fields_travel_little_endian(void)
{
    test_scalars_t in;
    static uint8_t buf[64];
    int8_t rc;
    uint8_t field;
    uint16_t len;

    memset(&in, 0, sizeof in);
    in.u16 = 0x1234u;
    in.i16 = -2;
    in.u32 = 0x89ABCDEFu;
    in.f32 = -2.0f;
    len = encode_here(&test_scalars_table, 0u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(35, len);
    TEST_ASSERT_EQUAL_HEX8(0x34u, buf[3]);
    TEST_ASSERT_EQUAL_HEX8(0x12u, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0xFEu, buf[5]);
    TEST_ASSERT_EQUAL_HEX8(0xFFu, buf[6]);
    TEST_ASSERT_EQUAL_HEX8(0xEFu, buf[7]);
    TEST_ASSERT_EQUAL_HEX8(0xCDu, buf[8]);
    TEST_ASSERT_EQUAL_HEX8(0xABu, buf[9]);
    TEST_ASSERT_EQUAL_HEX8(0x89u, buf[10]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[15]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[16]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[17]);
    TEST_ASSERT_EQUAL_HEX8(0xC0u, buf[18]);
}

/* covers: DSC-24 */
void test_codec_sixtyfour_bit_fields_copy_all_eight_bytes(void)
{
    test_scalars_t in;
    static uint8_t buf[64];
    test_scalars_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len;

    memset(&in, 0, sizeof in);
    memcpy(in.u64v, "\x00\x11\x22\x33\x44\x55\x66\x77", 8u);
    memcpy(in.i64v, "\xFF\xEE\xDD\xCC\xBB\xAA\x99\x88", 8u);
    len = encode_here(&test_scalars_table, 0u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(35, len);
    R2S_ASSERT_BYTES(in.u64v, 8u, buf + 19u, 8u, "u64 bytes");
    R2S_ASSERT_BYTES(in.i64v, 8u, buf + 27u, 8u, "i64 bytes");
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_scalars_table, 0u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
}

/* covers: DSC-23 */
void test_codec_bool_rejects_any_byte_but_zero_or_one(void)
{
    test_scalars_t in;
    static uint8_t buf[64];
    test_scalars_t out;
    int8_t rc;
    uint8_t field;

    memset(&in, 0, sizeof in);
    in.flag = 5u;
    encode_here(&test_scalars_table, 0u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_UINT8(0, field);
    memset(&in, 0, sizeof in);
    encode_here(&test_scalars_table, 0u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    buf[0] = 0x02u;
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_scalars_table, 0u, buf, 35u, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
}

/* covers: DSC-25, DSC-26, ENC-11 */
void test_codec_narrow_converts_float64_like_the_reference(void)
{
    /* Each row is the little-endian binary64 and the binary32 [ENC-11] makes
     * of it, taken from the reference codec. */
    static const struct {
        uint8_t in[8];
        uint8_t want[4];
    } rows[] = {
        /* 1.5 */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xF8u, 0x3Fu}, {0x00u, 0x00u, 0xC0u, 0x3Fu}},
        /* -2.0 */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xC0u}, {0x00u, 0x00u, 0x00u, 0xC0u}},
        /* tie down: 1 + 2^-24 keeps the even mantissa */
        {{0x00u, 0x00u, 0x00u, 0x10u, 0x00u, 0x00u, 0xF0u, 0x3Fu}, {0x00u, 0x00u, 0x80u, 0x3Fu}},
        /* tie up: 1 + 3 * 2^-24 is halfway between mantissas 1 and 2, and
         * keeps the even 2 */
        {{0x00u, 0x00u, 0x00u, 0x30u, 0x00u, 0x00u, 0xF0u, 0x3Fu}, {0x02u, 0x00u, 0x80u, 0x3Fu}},
        /* largest finite binary32 */
        {{0x00u, 0x00u, 0x00u, 0xE0u, 0xFFu, 0xFFu, 0xEFu, 0x47u}, {0xFFu, 0xFFu, 0x7Fu, 0x7Fu}},
        /* too large: an infinity of the same sign */
        {{0xB1u, 0xA1u, 0x16u, 0x2Au, 0xD3u, 0xCEu, 0xF2u, 0x47u}, {0x00u, 0x00u, 0x80u, 0x7Fu}},
        {{0xB1u, 0xA1u, 0x16u, 0x2Au, 0xD3u, 0xCEu, 0xF2u, 0xC7u}, {0x00u, 0x00u, 0x80u, 0xFFu}},
        /* smallest normal */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u, 0x38u}, {0x00u, 0x00u, 0x80u, 0x00u}},
        /* smallest subnormal */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xA0u, 0x36u}, {0x01u, 0x00u, 0x00u, 0x00u}},
        /* tie subnormal 1.5*2^-149 -> 2^-148 */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xA8u, 0x36u}, {0x02u, 0x00u, 0x00u, 0x00u}},
        /* 2^-129 exercises the shift-by-32 case */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xE0u, 0x37u}, {0x00u, 0x00u, 0x10u, 0x00u}},
        /* tie at 2^-150 rounds to zero */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x90u, 0x36u}, {0x00u, 0x00u, 0x00u, 0x00u}},
        /* below the tie: zero */
        {{0x61u, 0x55u, 0x2Cu, 0x24u, 0xCEu, 0x44u, 0x62u, 0x36u}, {0x00u, 0x00u, 0x00u, 0x00u}},
        /* zeros keep their sign */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x80u}, {0x00u, 0x00u, 0x00u, 0x80u}},
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}, {0x00u, 0x00u, 0x00u, 0x00u}},
        /* infinities cross unchanged */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xF0u, 0x7Fu}, {0x00u, 0x00u, 0x80u, 0x7Fu}},
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xF0u, 0xFFu}, {0x00u, 0x00u, 0x80u, 0xFFu}},
        /* binary64 normals below binary32 reach zero */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x10u, 0x00u}, {0x00u, 0x00u, 0x00u, 0x00u}},
        {{0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u}, {0x00u, 0x00u, 0x00u, 0x00u}},
        {{0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x80u}, {0x00u, 0x00u, 0x00u, 0x80u}},
        /* 1.0, 0.1 and a round-to-even above 2^24 */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xF0u, 0x3Fu}, {0x00u, 0x00u, 0x80u, 0x3Fu}},
        {{0x9Au, 0x99u, 0x99u, 0x99u, 0x99u, 0x99u, 0xB9u, 0x3Fu}, {0xCDu, 0xCCu, 0xCCu, 0x3Du}},
        {{0x00u, 0x00u, 0x00u, 0x10u, 0x00u, 0x00u, 0x70u, 0x41u}, {0x00u, 0x00u, 0x80u, 0x4Bu}},
        /* rounding at the top overflows to infinity */
        {{0x00u, 0x00u, 0x00u, 0xF0u, 0xFFu, 0xFFu, 0xEFu, 0x47u}, {0x00u, 0x00u, 0x80u, 0x7Fu}},
        /* subnormal ties: even stays, odd rounds up */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xB4u, 0x36u}, {0x02u, 0x00u, 0x00u, 0x00u}},
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xBCu, 0x36u}, {0x04u, 0x00u, 0x00u, 0x00u}},
        /* powers of two across the shift-by-32 case */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xF0u, 0x37u}, {0x00u, 0x00u, 0x20u, 0x00u}},
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x08u, 0x38u}, {0x00u, 0x00u, 0x60u, 0x00u}},
        {{0x00u, 0x00u, 0x00u, 0x40u, 0x00u, 0x00u, 0xF0u, 0x37u}, {0x00u, 0x00u, 0x20u, 0x00u}},
        /* a NaN stays a NaN */
        {{0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xF8u, 0x7Fu}, {0x00u, 0x00u, 0xC0u, 0x7Fu}},
    };
    static uint8_t buf[8];
    char label[64];
    uint16_t i;

    for (i = 0u; i < (uint16_t)(sizeof rows / sizeof rows[0]); i++) {
        test_f64_t in;
        test_f64_t back;
        int8_t rc;
        uint8_t field;
        uint16_t len;

        snprintf(label, sizeof label, "narrow row %u", (unsigned)i);
        memcpy(&in, rows[i].in, sizeof in);
        len = encode_here(&test_f64_table, 0u, &in, 1u, buf, (uint16_t)sizeof buf, &rc, &field);
        TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, rc, label);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(4u, len, label);
        R2S_ASSERT_BYTES(rows[i].want, 4u, buf, 4u, label);
        memset(&back, 0, sizeof back);
        TEST_ASSERT_EQUAL_INT8_MESSAGE(
            R2S_OK, decode_here(&test_f64_table, 0u, rows[i].want, 4u, 1u, &back, &field), label);
        len = encode_here(&test_f64_table, 0u, &back, 1u, buf, (uint16_t)sizeof buf, &rc, &field);
        TEST_ASSERT_EQUAL_INT8_MESSAGE(R2S_OK, rc, label);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(4u, len, label);
        R2S_ASSERT_BYTES(rows[i].want, 4u, buf, 4u, label);
    }
    /* Exact values widen back to themselves. */
    {
        static const uint16_t exacts[] = {0u, 1u, 4u, 7u, 8u, 13u, 14u, 15u, 16u, 20u};

        for (i = 0u; i < (uint16_t)(sizeof exacts / sizeof exacts[0]); i++) {
            test_f64_t back;
            uint8_t field;
            uint16_t row = exacts[i];

            snprintf(label, sizeof label, "widen row %u", (unsigned)row);
            TEST_ASSERT_EQUAL_INT8_MESSAGE(
                R2S_OK, decode_here(&test_f64_table, 0u, rows[row].want, 4u, 1u, &back, &field), label);
            TEST_ASSERT_EQUAL_MEMORY_MESSAGE(rows[row].in, &back, sizeof back, label);
        }
    }
    /* A member assignment round-trips through the conversion. */
    {
        test_f64_t probe;
        test_f64_t probe_back;
        uint8_t field = 0xFFu;
        uint16_t n;
        int8_t rc;

        probe.v = 0.5;
        n = encode_here(&test_f64_table, 0u, &probe, 1u, buf, (uint16_t)sizeof buf, &rc, &field);
        TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
        TEST_ASSERT_EQUAL_UINT16(4, n);
        memset(&probe_back, 0, sizeof probe_back);
        TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_f64_table, 0u, buf, 4u, 1u, &probe_back, &field));
        TEST_ASSERT_EQUAL_MEMORY(&probe, &probe_back, sizeof probe);
    }
}

/* covers: DSC-30 */
void test_codec_fixed_array_has_no_length_prefix(void)
{
    test_fixed_t in = {{1u, 2u, 3u}};
    static uint8_t buf[16];
    test_fixed_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len = encode_here(&test_arrays_table, 1u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);

    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(6, len);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x02u, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[3]);
    TEST_ASSERT_EQUAL_HEX8(0x03u, buf[4]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[5]);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_arrays_table, 1u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_EQUAL_UINT16(1u, out.v[0]);
    TEST_ASSERT_EQUAL_UINT16(2u, out.v[1]);
    TEST_ASSERT_EQUAL_UINT16(3u, out.v[2]);
}

/* covers: DSC-31, ENC-16 */
void test_codec_bounded_array_checks_its_bound(void)
{
    test_bounded_t in = {{0xAAu, 0xBBu, 0xCCu, 0xDDu}, 5u};
    static uint8_t buf[16];
    uint8_t payload[] = {0x05u, 0xAAu, 0xBBu, 0xCCu, 0xDDu, 0xEEu};
    test_bounded_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len;

    encode_here(&test_arrays_table, 2u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, decode_here(&test_arrays_table, 2u, payload,
                                                       (uint16_t)sizeof payload, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    in.n = 2u;
    len = encode_here(&test_arrays_table, 2u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(3, len);
    TEST_ASSERT_EQUAL_HEX8(0x02u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xAAu, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0xBBu, buf[2]);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_arrays_table, 2u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(2u, out.n);
    TEST_ASSERT_EQUAL_UINT8(0xAAu, out.v[0]);
    TEST_ASSERT_EQUAL_UINT8(0xBBu, out.v[1]);
}

/* covers: DSC-31 */
void test_codec_bounded_array_over_255_uses_a_two_byte_count(void)
{
    static test_bounded16_t in;
    static uint8_t buf[320];
    test_bounded16_t out;
    int8_t rc;
    uint8_t field;
    uint16_t i;
    uint16_t len;

    for (i = 0u; i < 256u; i++) {
        in.v[i] = 0xAAu;
    }
    in.n = 256u;
    len = encode_here(&test_arrays_table, 3u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(258, len);
    TEST_ASSERT_EQUAL_HEX8(0x80u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02u, buf[1]);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_arrays_table, 3u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT16(256, out.n);
    TEST_ASSERT_EQUAL_MEMORY(in.v, out.v, 256u);
    TEST_ASSERT_EQUAL_UINT8(0xAAu, out.v[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAAu, out.v[255]);
}

/* covers: DSC-32, ENC-14, ENC-15 */
void test_codec_string_needs_its_terminator_and_fits_its_bound(void)
{
    test_str_t in;
    static uint8_t buf[16];
    test_str_t out;
    uint8_t toolong[] = {0x06u, 'a', 'b', 'c', 'd', 'e', 'f'};
    uint8_t with_nul[] = {0x02u, 'h', 0x00u};
    int8_t rc;
    uint8_t field;
    uint16_t len;

    memset(&in, 0, sizeof in);
    memcpy(in.name, "hi", 3u);
    len = encode_here(&test_arrays_table, 0u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(3, len);
    TEST_ASSERT_EQUAL_HEX8(0x02u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8('h', buf[1]);
    TEST_ASSERT_EQUAL_HEX8('i', buf[2]);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_arrays_table, 0u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_STRING("hi", out.name);
    memset(&in, 'x', sizeof in.name);
    encode_here(&test_arrays_table, 0u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, decode_here(&test_arrays_table, 0u, toolong,
                                                       (uint16_t)sizeof toolong, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_arrays_table, 0u, with_nul,
                                                       (uint16_t)sizeof with_nul, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
}

/* covers: DSC-30, DSC-32, ENC-7, ENC-8 */
void test_codec_fixed_string_array_encodes_each_element(void)
{
    test_fixed_str_t in;
    static uint8_t buf[32];
    static const uint8_t want[] = {0x02u, 'h', 'i', 0x00u, 0x05u, 'a', 'b', 'c', 'd', 'e'};
    uint8_t toolong[] = {0x06u, 'a', 'b', 'c', 'd', 'e', 'f'};
    test_fixed_str_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len;

    memset(&in, 0, sizeof in);
    memcpy(in.name[0], "hi", 3u);
    memcpy(in.name[2], "abcde", 6u);
    len = encode_here(&test_arrays_table, 4u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)sizeof want, len);
    TEST_ASSERT_EQUAL_MEMORY(want, buf, sizeof want);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_arrays_table, 4u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_STRING("hi", out.name[0]);
    TEST_ASSERT_EQUAL_STRING("", out.name[1]);
    TEST_ASSERT_EQUAL_STRING("abcde", out.name[2]);
    memset(&in, 'x', sizeof in);
    encode_here(&test_arrays_table, 4u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, decode_here(&test_arrays_table, 4u, toolong,
                                                       (uint16_t)sizeof toolong, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
}

/* covers: DSC-31, DSC-32, ENC-7, ENC-9 */
void test_codec_bounded_string_array_round_trips(void)
{
    test_bounded_str_t in;
    static uint8_t buf[32];
    static const uint8_t want[] = {0x02u, 0x02u, 'h', 'i', 0x02u, 'a', 'b'};
    uint8_t overfull[] = {0x05u, 0x00u};
    uint8_t count_only[] = {0x01u};
    uint8_t short_element[] = {0x01u, 0x02u, 'a'};
    test_bounded_str_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len;

    memset(&in, 0, sizeof in);
    memcpy(in.data[0], "hi", 3u);
    memcpy(in.data[1], "ab", 3u);
    in.n = 2u;
    len = encode_here(&test_arrays_table, 5u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)sizeof want, len);
    TEST_ASSERT_EQUAL_MEMORY(want, buf, sizeof want);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_arrays_table, 5u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(2u, out.n);
    TEST_ASSERT_EQUAL_STRING("hi", out.data[0]);
    TEST_ASSERT_EQUAL_STRING("ab", out.data[1]);
    in.n = 5u;
    encode_here(&test_arrays_table, 5u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_UINT8(0, field);
    in.n = 2u;
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, decode_here(&test_arrays_table, 5u, overfull,
                                                       (uint16_t)sizeof overfull, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE,
                           decode_here(&test_arrays_table, 5u, overfull, 0u, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_arrays_table, 5u, count_only,
                                                       (uint16_t)sizeof count_only, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_arrays_table, 5u, short_element,
                                                       (uint16_t)sizeof short_element, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
}

/* covers: DSC-31, DSC-32, ENC-7, ENC-9 */
void test_codec_wide_bounded_string_array_round_trips(void)
{
    static test_bounded16_str_t in;
    static test_bounded16_str_t out;
    static uint8_t buf[520];
    uint8_t overfull[] = {0xADu, 0x02u};
    int8_t rc;
    uint8_t field;
    uint16_t len;
    uint16_t i;

    memset(&in, 0, sizeof in);
    for (i = 0u; i < 256u; i++) {
        in.data[i][0] = 'a';
    }
    in.n = 256u;
    len = encode_here(&test_arrays_table, 6u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(514, len);
    TEST_ASSERT_EQUAL_HEX8(0x80u, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x02u, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01u, buf[2]);
    TEST_ASSERT_EQUAL_HEX8('a', buf[3]);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_arrays_table, 6u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT16(256, out.n);
    TEST_ASSERT_EQUAL_STRING("a", out.data[0]);
    TEST_ASSERT_EQUAL_STRING("a", out.data[255]);
    in.n = 301u;
    encode_here(&test_arrays_table, 6u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, decode_here(&test_arrays_table, 6u, overfull,
                                                       (uint16_t)sizeof overfull, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
}

/* covers: DSC-30, DSC-31, DSC-32, ENC-7, ENC-8, ENC-9 */
void test_codec_two_string_arrays_name_their_own_field(void)
{
    test_str_pair_t in;
    static uint8_t buf[32];
    static const uint8_t want[] = {0x01u, 'a', 0x01u, 'b', 0x01u, 0x01u, 'c'};
    uint8_t overfull[] = {0x01u, 'a', 0x01u, 'b', 0x03u};
    test_str_pair_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len;

    memset(&in, 0, sizeof in);
    memcpy(in.fixed[0], "a", 2u);
    memcpy(in.fixed[1], "b", 2u);
    memcpy(in.data[0], "c", 2u);
    in.n = 1u;
    len = encode_here(&test_arrays_table, 7u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)sizeof want, len);
    TEST_ASSERT_EQUAL_MEMORY(want, buf, sizeof want);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_arrays_table, 7u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_STRING("a", out.fixed[0]);
    TEST_ASSERT_EQUAL_STRING("b", out.fixed[1]);
    TEST_ASSERT_EQUAL_UINT8(1u, out.n);
    TEST_ASSERT_EQUAL_STRING("c", out.data[0]);
    memset(in.data[1], 'x', sizeof in.data[1]);
    in.n = 2u;
    encode_here(&test_arrays_table, 7u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_UINT8(1, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, decode_here(&test_arrays_table, 7u, overfull,
                                                       (uint16_t)sizeof overfull, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(1, field);
}

/* covers: DSC-33 */
void test_codec_nested_message_inlines_its_fields(void)
{
    test_mid_t in;
    static uint8_t buf[8];
    test_mid_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len;

    in.in.tag = 0xABu;
    in.after = 0xCDu;
    len = encode_here(&test_nest_table, 1u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(2, len);
    TEST_ASSERT_EQUAL_HEX8(0xABu, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCDu, buf[1]);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_nest_table, 1u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_EQUAL_UINT8(0xABu, out.in.tag);
    TEST_ASSERT_EQUAL_UINT8(0xCDu, out.after);
}

/* covers: DSC-36, DSC-37 */
void test_codec_error_names_the_visited_field(void)
{
    static uint8_t buf[64];
    test_scalars_t in;
    test_mid_t mid;
    test_scalars_t sout;
    test_mid_t mout;
    int8_t rc;
    uint8_t field;
    uint16_t len;

    memset(&in, 0, sizeof in);
    mid.in.tag = 0xABu;
    mid.after = 0xCDu;
    len = encode_here(&test_scalars_table, 0u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(35, len);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_scalars_table, 0u, buf, 4u, 0u, &sout, &field));
    TEST_ASSERT_EQUAL_UINT8(3, field);
    len = encode_here(&test_nest_table, 1u, &mid, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_nest_table, 1u, buf, 1u, 0u, &mout, &field));
    TEST_ASSERT_EQUAL_UINT8(2, field);
    buf[len] = 0xFFu;
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE,
                           decode_here(&test_nest_table, 1u, buf, (uint16_t)(len + 1u), 0u, &mout, &field));
    TEST_ASSERT_EQUAL_UINT8(0xFF, field);
}

/* covers: DSC-40 */
void test_codec_nesting_deeper_than_the_stack_is_refused(void)
{
    test_outer_t in;
    static const uint8_t payload[] = {0xABu, 0xCDu, 0xEFu, 0x01u};
    static uint8_t buf[16];
    test_outer_t out;
    int8_t rc;
    uint8_t field;

    in.m1.in.tag = 0xABu;
    in.m1.after = 0xCDu;
    in.m2.in.tag = 0xEFu;
    in.m2.after = 0x01u;
    encode_here(&test_nest_table, 2u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_UINT8(1, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, decode_here(&test_nest_table, 2u, payload,
                                                       (uint16_t)sizeof payload, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT8(1, field);
}

/* covers: DSC-28 */
void test_codec_handwritten_types_round_trip(void)
{
    geometry_msgs__msg__Twist twist = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    std_msgs__msg__Float32 f32 = {1.5f};
    static uint8_t buf[64];
    geometry_msgs__msg__Twist twout;
    std_msgs__msg__Float32 fout;
    r2s_codec_out_t measure = {NULL, 0u, 0u, 0u};
    uint8_t field = 0xFFu;
    int8_t rc;
    uint16_t len;

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_TWIST, &twist, 0u,
                                                    &measure, &field));
    TEST_ASSERT_EQUAL_UINT16(48, measure.len);
    len = encode_here(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_TWIST, &twist, 0u, buf, (uint16_t)sizeof buf,
                      &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(48, len);
    memset(&twout, 0, sizeof twout);
    TEST_ASSERT_EQUAL_INT8(
        R2S_OK, decode_here(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_TWIST, buf, len, 0u, &twout, &field));
    TEST_ASSERT_EQUAL_MEMORY(&twist, &twout, sizeof twist);
    measure.len = 0u;
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_codec_encode(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_TWIST, &twist, 1u,
                                                    &measure, &field));
    TEST_ASSERT_EQUAL_UINT16(24, measure.len);
    len = encode_here(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_FLOAT32, &f32, 1u, buf, (uint16_t)sizeof buf,
                      &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(4, len);
    memset(&fout, 0, sizeof fout);
    TEST_ASSERT_EQUAL_INT8(
        R2S_OK, decode_here(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_FLOAT32, buf, 4u, 1u, &fout, &field));
    TEST_ASSERT_EQUAL_MEMORY(&f32, &fout, sizeof f32);
}

/* covers: DSC-28, DSC-30, DSC-31 */
void test_codec_nested_arrays_visit_each_element(void)
{
    test_holder_t in;
    static uint8_t buf[32];
    test_holder_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len;
    uint16_t i;

    memset(&in, 0, sizeof in);
    for (i = 0u; i < 2u; i++) {
        in.fx[i].tag = (uint8_t)(0xA0u + i);
        in.bd[i].tag = (uint8_t)(0xC0u + i);
    }
    in.b16[0].tag = 0xE0u;
    in.b16[1].tag = 0xE2u;
    in.b16_n = 2u;
    in.bd_n = 2u;
    len = encode_here(&test_nest_table, 3u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(8, len);
    memset(&out, 0, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_nest_table, 3u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_MEMORY(&in, &out, sizeof in);
    TEST_ASSERT_EQUAL_UINT8(0xE2u, out.b16[1].tag);
    TEST_ASSERT_EQUAL_UINT16(2u, out.b16_n);
    TEST_ASSERT_EQUAL_UINT8(2u, out.bd_n);
}

/* covers: DSC-31, ENC-16 */
void test_codec_nested_bounded_array_checks_its_bound(void)
{
    test_holder_t in;
    static uint8_t buf[32];
    uint8_t payload[] = {0xA0u, 0xA1u, 0xBDu, 0x02u};
    test_holder_t out;
    int8_t rc;
    uint8_t field;

    memset(&in, 0, sizeof in);
    in.b16_n = 301u;
    encode_here(&test_nest_table, 3u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, decode_here(&test_nest_table, 3u, payload,
                                                       (uint16_t)sizeof payload, 0u, &out, &field));
}

/* covers: DSC-31 */
void test_codec_empty_bounded_nested_array_sends_only_its_count(void)
{
    test_holder_t in;
    static uint8_t buf[32];
    test_holder_t out;
    int8_t rc;
    uint8_t field;
    uint16_t len;
    uint16_t i;

    memset(&in, 0, sizeof in);
    for (i = 0u; i < 2u; i++) {
        in.fx[i].tag = (uint8_t)(0xA0u + i);
    }
    len = encode_here(&test_nest_table, 3u, &in, 0u, buf, (uint16_t)sizeof buf, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, rc);
    TEST_ASSERT_EQUAL_UINT16(4, len);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00u, buf[3]);
    memset(&out, 0xFFu, sizeof out);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, decode_here(&test_nest_table, 3u, buf, len, 0u, &out, &field));
    TEST_ASSERT_EQUAL_UINT16(0u, out.b16_n);
    TEST_ASSERT_EQUAL_UINT8(0u, out.bd_n);
    TEST_ASSERT_EQUAL_UINT8(0xA1u, out.fx[1].tag);
}

/* covers: DSC-36 */
void test_codec_nested_array_truncated_varint_is_dropped(void)
{
    static const uint8_t payload[] = {0xA0u, 0xA1u, 0x80u};
    test_holder_t out;
    uint8_t field = 0xFFu;

    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_nest_table, 3u, payload,
                                                       (uint16_t)sizeof payload, 0u, &out, &field));
}

/* covers: DSC-35 */
void test_codec_buffer_too_small_fails_without_truncating(void)
{
    test_scalars_t scalars;
    test_str_t text;
    test_fixed_str_t fixed_text;
    test_bounded_str_t bounded_text;
    test_bounded_t bounded;
    test_f64_t wide;
    geometry_msgs__msg__Twist twist = {{1.5, -2.0, 0.5}, {1.0, -0.0, 3.0}};
    test_mid_t mid;
    static uint8_t buf[64];
    int8_t rc;
    uint8_t field;

    memset(&scalars, 0, sizeof scalars);
    encode_here(&test_scalars_table, 0u, &scalars, 0u, buf, 0u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    memset(&text, 0, sizeof text);
    encode_here(&test_arrays_table, 0u, &text, 0u, buf, 0u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    memset(&bounded, 0, sizeof bounded);
    bounded.n = 2u;
    encode_here(&test_arrays_table, 2u, &bounded, 0u, buf, 0u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    memset(&wide, 0, sizeof wide);
    encode_here(&test_f64_table, 0u, &wide, 1u, buf, 0u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    encode_here(&R2S_HANDWRITTEN_TABLE, R2S_HANDWRITTEN_TWIST, &twist, 0u, buf, 0u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    TEST_ASSERT_EQUAL_UINT8(1, field);
    mid.in.tag = 0xABu;
    mid.after = 0xCDu;
    encode_here(&test_nest_table, 1u, &mid, 0u, buf, 1u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    memset(&fixed_text, 0, sizeof fixed_text);
    memcpy(fixed_text.name[0], "hi", 3u);
    encode_here(&test_arrays_table, 4u, &fixed_text, 0u, buf, 0u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    encode_here(&test_arrays_table, 4u, &fixed_text, 0u, buf, 1u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    memset(&bounded_text, 0, sizeof bounded_text);
    memcpy(bounded_text.data[0], "hi", 3u);
    bounded_text.n = 1u;
    encode_here(&test_arrays_table, 5u, &bounded_text, 0u, buf, 0u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
    encode_here(&test_arrays_table, 5u, &bounded_text, 0u, buf, 1u, &rc, &field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, rc);
}

/* covers: DSC-36 */
void test_codec_truncated_payloads_report_their_entry(void)
{
    test_scalars_t sout;
    test_str_t strout;
    test_bounded_t bout;
    test_f64_t fout;
    test_mid_t mout;
    uint8_t field = 0xFFu;
    static const uint8_t one[] = {0x02u};
    static const uint8_t varint[] = {0x80u};
    static const uint8_t short_string[] = {0x05u, 0xAAu};
    static const uint8_t short_bounded[] = {0x02u, 0xAAu};
    static const uint8_t short_narrow[] = {0x00u, 0x00u};

    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_scalars_table, 0u, one, 0u, 0u, &sout, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_arrays_table, 0u, varint,
                                                       (uint16_t)sizeof varint, 0u, &strout, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_arrays_table, 2u, varint,
                                                       (uint16_t)sizeof varint, 0u, &bout, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_arrays_table, 0u, short_string,
                                                       (uint16_t)sizeof short_string, 0u, &strout, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_arrays_table, 2u, short_bounded,
                                                       (uint16_t)sizeof short_bounded, 0u, &bout, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_f64_table, 0u, short_narrow,
                                                       (uint16_t)sizeof short_narrow, 1u, &fout, &field));
    TEST_ASSERT_EQUAL_UINT8(0, field);
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_DECODE, decode_here(&test_nest_table, 1u, one, 0u, 0u, &mout, &field));
    TEST_ASSERT_EQUAL_UINT8(1, field);
}
