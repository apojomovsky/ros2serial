/* time: the MCU side of time sync (spec/protocol.md, section 7.2). The host
 * pings, the session answers with the delimiter tick, and corrections anchor
 * r2s_now() to ROS time. Compiled only with R2S_WITH_TIME. */
#ifndef R2S_TIME_H
#define R2S_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ROS time, the shape of builtin_interfaces/Time. r2s_now() returns {0, 0}
 * while no correction anchors the clock. */
typedef struct {
    int32_t sec;
    uint32_t nanosec;
} r2s_time_t;

/* How old an anchor may be before time reads unsynced (TIM-7). */
#ifndef R2S_TIME_MAX_ANCHOR_MS
#define R2S_TIME_MAX_ANCHOR_MS 10000u
#endif

/* A correction carries its drift inside these bounds (TIM-4) and its nanosec
 * below a second (TIM-11). The session refuses anything else, and the
 * arithmetic below refuses it the same way. */
#ifndef R2S_TIME_MAX_DRIFT_PPM
#define R2S_TIME_MAX_DRIFT_PPM 20000
#endif
#ifndef R2S_TIME_MAX_NANOSEC
#define R2S_TIME_MAX_NANOSEC 1000000000u
#endif

/* Stores a correction the session validated: tick0 is the tick the host
 * measured ROS time at (TIM-3). Later corrections replace the anchor. */
void r2s_time_note_correction(uint32_t tick0, int32_t sec0, uint32_t nanosec0, int16_t drift_ppm);

/* The spec section 7.2 arithmetic for the tick under the correction it
 * carries, in 32-bit integers alone (TIM-6, TIM-8, TIM-9). Inputs the wire
 * refuses read unsynced instead, leaving *out untouched: a tick_hz that is
 * not 1000 times a divisor of 10^6 (CTL-8), a drift_ppm outside +-20000
 * (TIM-4, TIM-5) and a nanosec0 of a second or more (TIM-11). Returns
 * nonzero with *out set, or 0 with *out untouched once the anchor is older
 * than R2S_TIME_MAX_ANCHOR_MS (TIM-7). */
uint8_t r2s_time_compute(uint32_t tick_hz, uint32_t tick0, int32_t sec0, uint32_t nanosec0, int16_t drift_ppm,
                         uint32_t tick, r2s_time_t *out);

#ifdef R2S_TESTING
/* Host test builds only: the same reset, as r2s_test_reset() calls it. */
void r2s_time_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
