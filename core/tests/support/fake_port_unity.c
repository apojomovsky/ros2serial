/* fake_port_fail() for Unity test executables. */
#include "fake_port.h"
#include "unity.h"

void fake_port_fail(const char *message)
{
    TEST_FAIL_MESSAGE(message);
}
