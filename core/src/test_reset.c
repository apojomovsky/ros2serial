/* test_reset: the host test builds' reset of every module's state. Each module
 * keeps its state in one static struct and adds a line here when it lands. */
#include "r2s/r2s.h"

#ifdef R2S_TESTING
void r2s_test_reset(void)
{
}
#else
/* ISO C needs at least one declaration in a translation unit. */
typedef int r2s_test_reset_unused;
#endif
