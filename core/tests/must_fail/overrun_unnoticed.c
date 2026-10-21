/* Must fail: a test that lets the core write past tx_free fails in tearDown,
 * even when it never looks at the overrun count itself. */
#include "fake_port.h"
#include "r2s/r2s_port.h"
#include "unity.h"

void test_must_fail_an_unnoticed_overrun_fails_the_test(void)
{
    fake_port_set_tx_free(1);
    r2s_port_write((const uint8_t *)"\x01\x02", 2);
}
