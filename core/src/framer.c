/* framer: bytes in through r2s_port_read, decoded frames out. Stub until
 * implemented. */
#include "r2s/r2s_framer.h"

static struct {
    r2s_framer_counters_t counters;
    uint8_t ready;
} framer;

void r2s_framer_init(void)
{
    framer.counters.rx_frames = 0u;
    framer.counters.crc_errors = 0u;
    framer.counters.rx_overflows = 0u;
    framer.counters.dropped_frames = 0u;
    framer.ready = 0u;
}

#ifdef R2S_TESTING
void r2s_framer_test_reset(void)
{
    r2s_framer_init();
}
#endif

uint8_t r2s_framer_poll(r2s_frame_t *frame)
{
    (void)frame;
    return framer.ready;
}

void r2s_framer_release(void)
{
    framer.ready = 0u;
}

const r2s_framer_counters_t *r2s_framer_counters(void)
{
    return &framer.counters;
}
