/* Must fail: a byte mismatch is reported with both strings in hex. CMake
 * passes this test only if the failure message appears. */
#include "bytes.h"
#include "unity.h"

void test_must_fail_assert_bytes_reports_a_mismatch(void)
{
    const uint8_t expected[] = {0x01, 0x02};
    const uint8_t actual[] = {0x01, 0x03, 0x04};

    R2S_ASSERT_BYTES(expected, 2, actual, 3, "demo");
}
