/* session: the MCU side of the handshake and the link. A stub that builds so
 * the handshake tests land first; the state machine arrives next. */
#include "r2s/r2s_session.h"

#include "r2s/r2s.h"

r2s_status_t r2s_session_configure(const char *node_name, const r2s_session_topic_t *topics,
                                   uint8_t topic_count)
{
    (void)node_name;
    (void)topics;
    (void)topic_count;
    return R2S_OK;
}

void r2s_init(void)
{
}

void r2s_spin_some(void)
{
}

uint8_t r2s_connected(void)
{
    return 0u;
}

#ifdef R2S_TESTING
void r2s_session_test_reset(void)
{
}
#endif
