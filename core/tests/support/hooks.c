/* Unity calls these around every test: each one starts from a reset core and
 * a reset fake port, so tests pass alone and in any order. */
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
}
