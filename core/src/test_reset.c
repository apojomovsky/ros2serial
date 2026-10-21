/* test_reset: the host test builds' reset of every module's state. A stateful
 * module keeps its state in one static struct, defines r2s_<module>_test_reset()
 * under R2S_TESTING next to it, and adds the call here when it lands. */
#include "r2s/r2s.h"

#ifdef R2S_TESTING
void r2s_test_reset(void)
{
}
#else
/* ISO C needs at least one declaration in a translation unit. */
typedef int r2s_test_reset_unused;
#endif
