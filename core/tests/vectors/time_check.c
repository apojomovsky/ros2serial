/* Conformance: every time vector through the core's time arithmetic. The
 * check drives the same function r2s_now() uses, with the vector's tick rate,
 * so the C core and the Python reference agree case by case. */
#include <stdio.h>

#include "r2s/r2s_time.h"
#include "unity.h"
#include "vectors.h"

/* covers: TIM-4, TIM-6, TIM-7, TIM-8, TIM-9 */
void r2s_vec_check_time(const r2s_vec_time_t *v)
{
    char label[256];
    r2s_time_t got = {0, 0};
    uint8_t synced;

    snprintf(label, sizeof label, "%s [%s]", v->where, v->covers);
    synced = r2s_time_compute(v->tick_hz, v->tick0, v->sec0, v->nanosec0, v->drift_ppm, v->tick, &got);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(v->synced, synced, label);
    /* Unsynced reads {0, 0}: the check initializes got that way, so a
     * regression that sets a time while unsynced fails here. */
    TEST_ASSERT_EQUAL_INT32_MESSAGE(v->sec, got.sec, label);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(v->nanosec, got.nanosec, label);
}
