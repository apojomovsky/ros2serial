/* rCOBS: the encoding round-trips, it never holds a 0x00, the decoder accepts
 * only the canonical encoding, and the decoder decodes in place. */
#include "bytes.h"
#include "r2s/r2s_rcobs.h"
#include "unity.h"

/* The most bytes an encoding of n bytes takes (COB-8). */
#define R2S_RCOBS_BOUND(n) ((n) + (n) / 254u + 1u)

/* Encodes data into out and returns its length, which is at most
 * R2S_RCOBS_MAX_FRAME + its bound. */
static uint16_t encode(const uint8_t *data, uint16_t len, uint8_t *out)
{
    r2s_rcobs_encoder_t enc;
    uint16_t written = 0;

    r2s_rcobs_encoder_init(&enc);
    for (uint16_t i = 0; i < len; i++) {
        written = (uint16_t)(written + r2s_rcobs_encode_byte(&enc, data[i], out + written));
    }
    return (uint16_t)(written + r2s_rcobs_encode_end(&enc, out + written));
}

/* covers: COB-1, COB-2, COB-4, COB-7 */
void test_rcobs_a_frame_round_trips(void)
{
    const uint8_t data[] = {0x11, 0x22, 0x00, 0x33, 0x00, 0x00};
    uint8_t encoded[32];
    uint8_t *decoded = NULL;
    uint16_t decoded_len = 0;
    uint16_t len = encode(data, sizeof data, encoded);

    TEST_ASSERT_EQUAL_INT8(R2S_RCOBS_OK, r2s_rcobs_decode(encoded, len, &decoded, &decoded_len));
    TEST_ASSERT_EQUAL_UINT16(sizeof data, decoded_len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, decoded, sizeof data);
}

/* covers: COB-1 */
void test_rcobs_an_empty_frame_encodes_as_one_byte(void)
{
    uint8_t encoded[4];
    uint16_t len = encode(NULL, 0, encoded);

    TEST_ASSERT_EQUAL_UINT16(1, len);
    TEST_ASSERT_EQUAL_HEX8(0x01, encoded[0]);
}

/* covers: COB-2 */
void test_rcobs_the_encoding_never_holds_a_zero_byte(void)
{
    uint8_t data[300];
    uint8_t encoded[400];

    for (unsigned i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i % 7u);
    }
    uint16_t len = encode(data, sizeof data, encoded);

    for (uint16_t i = 0; i < len; i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, encoded[i], "the encoding holds a zero byte");
    }
}

/* covers: COB-8 */
void test_rcobs_the_encoding_stays_within_the_bound(void)
{
    uint8_t data[600];
    uint8_t encoded[700];

    for (unsigned i = 0; i < sizeof data; i++) {
        data[i] = 0x11;
    }
    uint16_t len = encode(data, sizeof data, encoded);

    TEST_ASSERT_EQUAL_UINT16(R2S_RCOBS_BOUND(sizeof data), len);
}

/* covers: COB-3 */
void test_rcobs_254_nonzero_bytes_at_the_end_give_a_trailing_empty_group(void)
{
    uint8_t data[254];
    uint8_t encoded[300];

    for (unsigned i = 0; i < sizeof data; i++) {
        data[i] = 0x11;
    }
    uint16_t len = encode(data, sizeof data, encoded);

    TEST_ASSERT_EQUAL_HEX8(0xFF, encoded[len - 2u]);
    TEST_ASSERT_EQUAL_HEX8(0x01, encoded[len - 1u]);
}

/* covers: COB-4, COB-7 */
void test_rcobs_a_group_of_254_bytes_ends_itself(void)
{
    uint8_t data[254];
    uint8_t encoded[300];

    for (unsigned i = 0; i < sizeof data; i++) {
        data[i] = 0x11;
    }
    uint16_t len = encode(data, sizeof data, encoded);

    /* The 254 bytes, the code byte of the group and the empty group that
     * follows it. */
    TEST_ASSERT_EQUAL_UINT16(256, len);
    TEST_ASSERT_EQUAL_HEX8(0xFF, encoded[254]);
    TEST_ASSERT_EQUAL_HEX8(0x01, encoded[255]);
}

/* covers: COB-4 */
void test_rcobs_decoding_ends_where_the_encoding_ended(void)
{
    const uint8_t data[] = {0x11, 0x22, 0x00, 0x33};
    uint8_t encoded[32];
    uint8_t *decoded = NULL;
    uint16_t decoded_len = 0;
    uint16_t len = encode(data, sizeof data, encoded);

    TEST_ASSERT_EQUAL_INT8(R2S_RCOBS_OK, r2s_rcobs_decode(encoded, len, &decoded, &decoded_len));
    /* The decoded frame ends where the encoded one did, so its last byte is
     * the last byte of the encoding. */
    TEST_ASSERT_EQUAL_UINT8(0x33, decoded[sizeof data - 1u]);
    TEST_ASSERT_EQUAL_UINT8(0x33, encoded[len - 1u]);
}

/* covers: COB-5 */
void test_rcobs_a_zero_byte_in_the_encoding_is_refused(void)
{
    /* The codes are sound, so only the 0x00 byte itself can refuse this. */
    uint8_t encoded[] = {0x00, 0x02, 0x01};
    uint8_t *decoded = NULL;
    uint16_t decoded_len = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_RCOBS_ERR_CODE,
                           r2s_rcobs_decode(encoded, sizeof encoded, &decoded, &decoded_len));
}

/* covers: COB-5 */
void test_rcobs_a_code_without_its_bytes_is_refused(void)
{
    uint8_t encoded[] = {0x11, 0x03};
    uint8_t *decoded = NULL;
    uint16_t decoded_len = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_RCOBS_ERR_CODE,
                           r2s_rcobs_decode(encoded, sizeof encoded, &decoded, &decoded_len));
}

/* covers: COB-6 */
void test_rcobs_a_trailing_0xff_code_is_refused(void)
{
    uint8_t encoded[] = {0x11, 0xFF};
    uint8_t *decoded = NULL;
    uint16_t decoded_len = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_RCOBS_ERR_TRAILING,
                           r2s_rcobs_decode(encoded, sizeof encoded, &decoded, &decoded_len));
}

/* covers: COB-6 */
void test_rcobs_the_trailing_rule_is_checked_before_any_code(void)
{
    /* The last byte breaks COB-6 and the first one breaks COB-5; COB-6 comes
     * first, so that is the rule the decoder names. */
    uint8_t encoded[] = {0x00, 0xFF};
    uint8_t *decoded = NULL;
    uint16_t decoded_len = 0;

    TEST_ASSERT_EQUAL_INT8(R2S_RCOBS_ERR_TRAILING,
                           r2s_rcobs_decode(encoded, sizeof encoded, &decoded, &decoded_len));
}

/* covers: COB-8 */
void test_rcobs_a_frame_longer_than_a_pointer_difference_decodes(void)
{
    /* FRM-9: the protocol allows a frame of 65277 bytes. A pointer
     * difference is 16 bits on an AVR, so the length comes from the
     * decoder. */
    static uint8_t data[40000];
    static uint8_t encoded[40000 + 40000 / 254 + 2];
    uint8_t *decoded = NULL;
    uint16_t decoded_len = 0;

    for (unsigned i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)((i % 251u) + 1u);
    }
    uint16_t len = encode(data, sizeof data, encoded);

    TEST_ASSERT_EQUAL_INT8(R2S_RCOBS_OK, r2s_rcobs_decode(encoded, len, &decoded, &decoded_len));
    TEST_ASSERT_EQUAL_UINT16(sizeof data, decoded_len);
    R2S_ASSERT_BYTES(data, sizeof data, decoded, decoded_len, "a long frame");
}

/* covers: COB-3, COB-6 */
void test_rcobs_decoding_accepts_only_the_canonical_encoding(void)
{
    uint8_t data[300];
    uint8_t encoded[300];
    uint8_t group_of_254[254];

    /* The shapes that make the decoder work hardest: no zero byte, a zero
     * byte at each end, and a group of 254 bytes. */
    const uint8_t zero_at_start[] = {0x00, 0x01, 0x02, 0x03};
    const uint8_t zero_at_end[] = {0x01, 0x02, 0x03, 0x00};

    for (unsigned i = 0; i < sizeof group_of_254; i++) {
        group_of_254[i] = 0x11;
    }

    for (unsigned shape = 0; shape < 4u; shape++) {
        uint16_t data_len;
        const uint8_t *source = data;

        if (shape == 0u) {
            for (unsigned i = 0; i < 64u; i++) {
                data[i] = (uint8_t)(i + 1u);
            }
            data_len = 64u;
        } else if (shape == 1u) {
            source = zero_at_start;
            data_len = sizeof zero_at_start;
        } else if (shape == 2u) {
            source = zero_at_end;
            data_len = sizeof zero_at_end;
        } else {
            source = group_of_254;
            data_len = sizeof group_of_254;
        }
        uint16_t len = encode(source, data_len, encoded);

        /* Any encoding of the same bytes that is not the one the encoder
         * made would give the decoder a second reading, so it must be
         * refused. */
        for (uint16_t i = 0; i < len; i++) {
            uint8_t original[300];
            uint8_t buffer[300];
            uint8_t *decoded = NULL;
            uint16_t decoded_len = 0;

            for (uint16_t k = 0; k < len; k++) {
                original[k] = encoded[k];
            }
            original[i] = (uint8_t)(original[i] + 1u);
            for (uint16_t k = 0; k < len; k++) {
                buffer[k] = original[k];
            }

            int8_t status = r2s_rcobs_decode(buffer, len, &decoded, &decoded_len);
            if (status == R2S_RCOBS_OK) {
                uint8_t reencoded[300];
                r2s_rcobs_encoder_t enc;
                uint16_t reencoded_len = 0;

                r2s_rcobs_encoder_init(&enc);
                for (uint16_t k = 0; k < decoded_len; k++) {
                    reencoded_len =
                        (uint16_t)(reencoded_len +
                                   r2s_rcobs_encode_byte(&enc, decoded[k], reencoded + reencoded_len));
                }
                reencoded_len =
                    (uint16_t)(reencoded_len + r2s_rcobs_encode_end(&enc, reencoded + reencoded_len));
                TEST_ASSERT_EQUAL_UINT16_MESSAGE(len, reencoded_len, "a second encoding of the same bytes");
                for (uint16_t k = 0; k < len; k++) {
                    TEST_ASSERT_EQUAL_HEX8_MESSAGE(original[k], reencoded[k],
                                                   "a second encoding of the same bytes");
                }
            }
        }
    }
}
