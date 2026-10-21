/* Unity calls these around every test: each one starts from a reset core and
 * a reset fake port, and fails if the core wrote past tx_free unnoticed. */
#include "fake_port.h"
#include "r2s/r2s.h"
#include "unity.h"

void setUp(void)
{
    r2s_test_reset();
    fake_port_reset();
}

void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, fake_port_take_overruns(), "core wrote past tx_free");
}
