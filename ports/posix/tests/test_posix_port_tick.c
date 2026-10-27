/* Port tick checks: monotonic, live and at 1000 Hz. It links only the port,
 * so it runs with no pty and no boot file. */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "r2s/r2s_port.h"

#define SAMPLES 100000u

static void sleep_ms(long ms)
{
    const struct timespec req = {ms / 1000L, (ms % 1000L) * 1000000L};

    (void)nanosleep(&req, NULL);
}

/* Wall time from the same clock the port uses, so scheduling slips move both
 * measures together and only a wrong rate changes their ratio. */
static uint64_t wall_ms(void)
{
    struct timespec now;

    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000L);
}

static int check_monotonic(void)
{
    uint32_t prev = r2s_port_tick();
    uint32_t i;

    for (i = 0u; i < SAMPLES; i++) {
        uint32_t now = r2s_port_tick();

        if (now < prev) {
            fprintf(stderr, "tick went back from %lu to %lu\n", (unsigned long)prev, (unsigned long)now);
            return 1;
        }
        prev = now;
    }
    return 0;
}

int main(void)
{
    uint32_t before;
    uint32_t after;
    uint64_t wall_before;
    uint64_t wall_after;
    uint64_t elapsed;
    uint64_t span;

    if (check_monotonic() != 0) {
        return 1;
    }
    before = r2s_port_tick();
    sleep_ms(5L);
    after = r2s_port_tick();
    if (after <= before) {
        fprintf(stderr, "tick did not advance in 5 ms\n");
        return 1;
    }
    wall_before = wall_ms();
    before = r2s_port_tick();
    sleep_ms(200L);
    wall_after = wall_ms();
    after = r2s_port_tick();
    elapsed = wall_after - wall_before;
    span = (uint64_t)(after - before);
    if (elapsed < 100u) {
        fprintf(stderr, "wall clock did not advance\n");
        return 1;
    }
    if (span * 5u < elapsed * 4u || span * 5u > elapsed * 6u) {
        fprintf(stderr, "tick moved %lu in %lu ms, want a 1:1 ratio\n", (unsigned long)span,
                (unsigned long)elapsed);
        return 1;
    }
    printf("tick ok: monotonic, live, 1000 Hz\n");
    return 0;
}
