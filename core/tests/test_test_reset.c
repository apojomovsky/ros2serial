/* r2s_test_reset(): every stateful core module adds its reset and a
 * dirty-reset-check test here when it lands (C5 onwards). */
#include "r2s/r2s.h"
#include "unity.h"

void test_test_reset_is_callable_in_test_builds(void)
{
    r2s_test_reset();
}
