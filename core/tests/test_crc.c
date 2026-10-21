/* CRC-16/MCRF4XX: the published check value, the zero residue and the
 * incremental and buffer forms agreeing. */
#include <stdio.h>

#include "r2s/r2s_crc.h"
#include "unity.h"

/* A fixed seed on every PR, printed so a failure reproduces from it alone; the
 * nightly build passes a fresh one with -DR2S_TEST_SEED. */
#ifndef R2S_TEST_SEED
#define R2S_TEST_SEED 20261021u
#endif
#define SEED R2S_TEST_SEED

static uint32_t rng_state;

static uint8_t next_byte(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (uint8_t)(rng_state >> 24);
}

/* covers: CHK-1, CHK-2 */
void test_crc_check_string_gives_the_published_value(void)
{
    TEST_ASSERT_EQUAL_HEX16(0x6F91, r2s_crc16_update_buf(R2S_CRC16_INIT, (const uint8_t *)"123456789", 9));
}

void test_crc_no_bytes_leave_the_initial_value(void)
{
    TEST_ASSERT_EQUAL_HEX16(R2S_CRC16_INIT, r2s_crc16_update_buf(R2S_CRC16_INIT, NULL, 0));
}

/* covers: CHK-3, CHK-4 */
void test_crc_appended_little_endian_leaves_a_zero_residue(void)
{
    uint8_t frame[66];
    char seed[48];

    rng_state = SEED;
    snprintf(seed, sizeof seed, "seed %u", SEED);
    TEST_MESSAGE(seed);
    for (unsigned trial = 0; trial < 2000u; trial++) {
        uint16_t len = (uint16_t)(next_byte() % 64u);
        for (uint16_t i = 0; i < len; i++) {
            frame[i] = next_byte();
        }
        uint16_t crc = r2s_crc16_update_buf(R2S_CRC16_INIT, frame, len);
        frame[len] = (uint8_t)(crc & 0xFFu);
        frame[len + 1u] = (uint8_t)(crc >> 8);
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(0, r2s_crc16_update_buf(R2S_CRC16_INIT, frame, (uint16_t)(len + 2u)),
                                        seed);
    }
}

void test_crc_one_byte_at_a_time_equals_the_buffer_form(void)
{
    const uint8_t data[] = {0x10, 0x00, 0x00, 0x00, 0xc0, 0x3f};
    uint16_t crc = R2S_CRC16_INIT;

    for (unsigned i = 0; i < sizeof data; i++) {
        crc = r2s_crc16_update(crc, data[i]);
    }

    TEST_ASSERT_EQUAL_HEX16(r2s_crc16_update_buf(R2S_CRC16_INIT, data, sizeof data), crc);
}

void test_crc_split_anywhere_equals_one_pass(void)
{
    uint8_t data[300];

    rng_state = SEED;
    for (unsigned i = 0; i < sizeof data; i++) {
        data[i] = next_byte();
    }
    uint16_t whole = r2s_crc16_update_buf(R2S_CRC16_INIT, data, sizeof data);
    for (uint16_t k = 0; k <= sizeof data; k++) {
        uint16_t head = r2s_crc16_update_buf(R2S_CRC16_INIT, data, k);
        TEST_ASSERT_EQUAL_HEX16(whole, r2s_crc16_update_buf(head, data + k, (uint16_t)(sizeof data - k)));
    }
}

/* covers: CHK-4 */
void test_crc_residue_holds_at_the_largest_frame(void)
{
    /* FRM-9: 65277 bytes from topic_id to crc16. */
    static uint8_t frame[65277];
    uint16_t len = (uint16_t)(sizeof frame - 2u);

    rng_state = SEED;
    for (uint16_t i = 0; i < len; i++) {
        frame[i] = next_byte();
    }
    uint16_t crc = r2s_crc16_update_buf(R2S_CRC16_INIT, frame, len);
    frame[len] = (uint8_t)(crc & 0xFFu);
    frame[len + 1u] = (uint8_t)(crc >> 8);

    TEST_ASSERT_EQUAL_HEX16(0, r2s_crc16_update_buf(R2S_CRC16_INIT, frame, (uint16_t)sizeof frame));
}
