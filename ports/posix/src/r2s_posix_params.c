/* The virtual MCU's parameter table and its stderr reporter. */
#include "r2s_posix_params.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "r2s/r2s.h"
#include "r2s/r2s_params.h"

/* The table the bridge walks: led at index 0, rate at index 1. */
static r2s_param_t s_table[] = {
    {"led", R2S_PARAM_BOOL, 0u, 0u, {.b = 1u}, {.b = 0u}},
    {"rate", R2S_PARAM_I64, 0u, 0u, {.i = 2}, {.i = 0}},
};

#define PARAM_COUNT (sizeof(s_table) / sizeof(s_table[0]))

/* The values the last report covered, seeded from the live ones so only
 * applied SETs print. */
static uint8_t s_led;
static int64_t s_rate;
static uint8_t s_seeded = 0u;

int r2s_posix_params_init(void)
{
    uint8_t count = (uint8_t)PARAM_COUNT;

    if (count != PARAM_COUNT || r2s_params_configure(s_table, count) != R2S_OK) {
        return -1;
    }
    s_led = s_table[0].value.b;
    s_rate = s_table[1].value.i;
    s_seeded = 1u;
    return 0;
}

void r2s_posix_params_report(void)
{
    const r2s_param_t *led;
    const r2s_param_t *rate;

    if (s_seeded == 0u) {
        return;
    }
    led = r2s_params_get(0u);
    rate = r2s_params_get(1u);
    if (led == NULL || rate == NULL) {
        return;
    }
    if (led->value.b != s_led) {
        s_led = led->value.b;
        fprintf(stderr, "r2s_posix: param led = %u\n", (unsigned int)s_led);
    }
    if (rate->value.i != s_rate) {
        s_rate = rate->value.i;
        fprintf(stderr, "r2s_posix: param rate = %" PRId64 "\n", s_rate);
    }
}
