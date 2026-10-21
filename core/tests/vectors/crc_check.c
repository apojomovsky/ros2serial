/* Conformance: every CRC vector through the core's CRC. */
#include <string.h>

#include "r2s/r2s_crc.h"
#include "unity.h"
#include "vectors.h"

void r2s_vec_check_crc(const r2s_vec_crc_t *v)
{
    char label[256];

    strncpy(label, v->where, sizeof label - 1u);
    label[sizeof label - 1u] = '\0';
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(v->crc, r2s_crc16_update_buf(R2S_CRC16_INIT, v->data, v->data_len),
                                    label);
}
