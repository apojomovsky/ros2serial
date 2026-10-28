/* time: the MCU side of time sync (spec/protocol.md, section 7.2). The host
 * pings, the session answers with the delimiter tick, and corrections anchor
 * r2s_now() to ROS time. */
#include "r2s/r2s.h"
#include "r2s/r2s_time.h"

#ifdef R2S_WITH_TIME

r2s_time_t r2s_now(void)
{
    r2s_time_t out = {0, 0};

    return out;
}

uint8_t r2s_time_synced(void)
{
    return 0u;
}

void r2s_time_note_correction(uint32_t tick0, int32_t sec0, uint32_t nanosec0, int16_t drift_ppm)
{
    (void)tick0;
    (void)sec0;
    (void)nanosec0;
    (void)drift_ppm;
}

uint8_t r2s_time_compute(uint32_t tick_hz, uint32_t tick0, int32_t sec0, uint32_t nanosec0, int16_t drift_ppm,
                         uint32_t tick, r2s_time_t *out)
{
    (void)tick_hz;
    (void)tick0;
    (void)sec0;
    (void)nanosec0;
    (void)drift_ppm;
    (void)tick;
    (void)out;
    return 0u;
}

#ifdef R2S_TESTING
void r2s_time_test_reset(void)
{
}
#endif

#endif
