/* Unity calls these around every test: each one starts from a reset core,
 * a reset fake port and empty registries, and fails if the core wrote past
 * tx_free unnoticed. The core's own hooks reset the core; the runtime and
 * action registries are this layer's, so they reset here. */
/* The fake port's header carries no C linkage guard, and this file is C++. */
extern "C" {
#include "fake_port.h"
}
#include "r2s/r2s.h"
#include "unity.h"

extern "C" void r2s_runtime_test_reset(void);
extern "C" void r2s_action_test_reset(void);
extern "C" void r2s_service_test_reset(void);

extern "C" void setUp(void)
{
  r2s_test_reset();
  fake_port_reset();
  r2s_runtime_test_reset();
  r2s_action_test_reset();
  r2s_service_test_reset();
}

extern "C" void tearDown(void)
{
  TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, fake_port_take_overruns(), "core wrote past tx_free");
}
