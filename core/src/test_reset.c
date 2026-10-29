/* test_reset: the host test builds' reset of every module's state. A stateful
 * module keeps its state in one static struct, defines r2s_<module>_test_reset()
 * under R2S_TESTING next to it, and adds the call here when it lands. */
#include "r2s/r2s.h"

#include "r2s/r2s_framer.h"
#include "r2s/r2s_rx.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"
#include "r2s/r2s_tx.h"

#ifdef R2S_TESTING
void r2s_test_reset(void)
{
    r2s_session_test_reset();
    r2s_topics_test_reset();
    r2s_framer_test_reset();
    r2s_rx_test_reset();
    r2s_tx_test_reset();
#ifdef R2S_WITH_LOG
    r2s_log_test_reset();
#endif
#ifdef R2S_WITH_TIME
    r2s_time_test_reset();
#endif
}
#else
/* ISO C needs at least one declaration in a translation unit. */
typedef int r2s_test_reset_unused;
#endif
