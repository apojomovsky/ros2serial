/* Must fail: the vector self-test rejects a record whose bytes disagree with
 * its hex text, as a mistranscribed vector would. */
#include "unity.h"
#include "vectors.h"

void test_must_fail_selftest_rejects_mistranscribed_bytes(void)
{
    static const uint8_t decoded[] = {0x11, 0x23};
    static const uint8_t encoded[] = {0x11, 0x22, 0x03, 0x00};
    const r2s_vec_rcobs_t v = {"must_fail case 0", "COB-1", decoded, 2u, "11 22", encoded, 4u, "11 22 03 00"};

    r2s_vec_check_rcobs(&v);
}
