/* time: the MCU side of time sync (spec/protocol.md, section 7.2). The host
 * pings, the session answers with the delimiter tick, and corrections anchor
 * r2s_now() to ROS time. */
#include "r2s/r2s.h"

#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_time.h"

#ifdef R2S_WITH_TIME

/* tick_hz is 1000 times a divisor of 10^6, so a tick converts to nanoseconds
 * with integer math alone (CTL-8). A failing build names the rule here. */
typedef char r2s_time_tick_hz_form[(R2S_TICK_HZ >= 1000u && R2S_TICK_HZ % 1000u == 0u &&
                                    (1000000u % (R2S_TICK_HZ / 1000u)) == 0u)
                                       ? 1
                                       : -1];

static struct {
    uint32_t tick0;
    int32_t sec0;
    uint32_t nanosec0;
    int16_t drift_ppm;
    uint8_t anchored;
} anchor;

uint8_t r2s_time_compute(uint32_t tick_hz, uint32_t tick0, int32_t sec0, uint32_t nanosec0, int16_t drift_ppm,
                         uint32_t tick, r2s_time_t *out)
{
    /* Ticks per millisecond and nanoseconds per tick: both divide evenly, so
     * the elapsed time splits into whole milliseconds and a remainder. */
    uint32_t k = tick_hz / 1000u;
    uint32_t p;
    uint32_t e;
    uint32_t ms;
    uint32_t r;
    uint32_t ns;
    int32_t drifted;
    int32_t sec;

    /* Inputs the wire refuses read unsynced instead. */
    if (tick_hz < 1000u || tick_hz % 1000u != 0u || k == 0u || 1000000u % k != 0u) {
        return 0u;
    }
    if (drift_ppm > R2S_TIME_MAX_DRIFT_PPM || drift_ppm < -R2S_TIME_MAX_DRIFT_PPM ||
        nanosec0 >= R2S_TIME_MAX_NANOSEC) {
        return 0u;
    }
    p = 1000000u / k;
    e = tick - tick0;
    ms = e / k;
    r = e % k;
    if (ms > R2S_TIME_MAX_ANCHOR_MS) {
        return 0u;
    }
    /* nanosec0 is below 10^9, (ms % 1000) * 10^6 below 10^9 and r * p below
     * 10^6, so the sum stays below 2^32 (TIM-8). */
    ns = nanosec0 + (ms % 1000u) * 1000000u + r * p;
    /* The second counts up in unsigned arithmetic, so a correction near the
     * int32 ends wraps the way two's complement does instead of trapping. */
    sec = (int32_t)((uint32_t)sec0 + ms / 1000u + ns / 1000000000u);
    ns = ns % 1000000000u;
    /* ns is below 10^9 and the drift term within +-2 * 10^8 in magnitude, so
     * their sum fits int32 (TIM-8). */
    drifted = (int32_t)ns + (int32_t)ms * (int32_t)drift_ppm;
    if (drifted < 0) {
        sec = (int32_t)((uint32_t)sec - 1u);
        ns = (uint32_t)(drifted + 1000000000);
    } else if (drifted >= 1000000000) {
        sec = (int32_t)((uint32_t)sec + 1u);
        ns = (uint32_t)(drifted - 1000000000);
    } else {
        ns = (uint32_t)drifted;
    }
    out->sec = sec;
    out->nanosec = ns;
    return 1u;
}

void r2s_time_note_correction(uint32_t tick0, int32_t sec0, uint32_t nanosec0, int16_t drift_ppm)
{
    anchor.tick0 = tick0;
    anchor.sec0 = sec0;
    anchor.nanosec0 = nanosec0;
    anchor.drift_ppm = drift_ppm;
    anchor.anchored = 1u;
}

r2s_time_t r2s_now(void)
{
    r2s_time_t out = {0, 0};

    if (anchor.anchored != 0u) {
        (void)r2s_time_compute(R2S_TICK_HZ, anchor.tick0, anchor.sec0, anchor.nanosec0, anchor.drift_ppm,
                               r2s_port_tick(), &out);
    }
    return out;
}

uint8_t r2s_time_synced(void)
{
    r2s_time_t ignored = {0, 0};

    if (anchor.anchored == 0u) {
        return 0u;
    }
    return r2s_time_compute(R2S_TICK_HZ, anchor.tick0, anchor.sec0, anchor.nanosec0, anchor.drift_ppm,
                            r2s_port_tick(), &ignored);
}

#ifdef R2S_TESTING
void r2s_time_test_reset(void)
{
    anchor.tick0 = 0u;
    anchor.sec0 = 0;
    anchor.nanosec0 = 0u;
    anchor.drift_ppm = 0;
    anchor.anchored = 0u;
}
#endif

#endif
