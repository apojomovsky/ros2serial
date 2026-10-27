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
    uint32_t span;

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
    sleep_ms(100L);
    span = r2s_port_tick() - after;
    if (span < 50u || span > 500u) {
        fprintf(stderr, "tick moved %lu in 100 ms, want 50..500\n", (unsigned long)span);
        return 1;
    }
    printf("tick ok: monotonic, live, 1000 Hz\n");
    return 0;
}
