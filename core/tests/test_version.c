/* The protocol version the core announces. */
#include "r2s/r2s.h"
#include "unity.h"

/* covers: GEN-2 */
void test_version_protocol_is_2(void)
{
    TEST_ASSERT_EQUAL_UINT8(2, R2S_PROTOCOL_VERSION);
}
