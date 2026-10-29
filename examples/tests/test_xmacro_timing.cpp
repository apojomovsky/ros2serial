/* The X-macro example resyncs without bursts: a slightly late tick keeps
 * the phase, a stale deadline publishes once and resyncs, and the offline
 * branch resets the deadline instead of replaying it. Each case drives the
 * real sketch with a frozen tick, so spacing is asserted on the firmware
 * clock. A stale-deadline burst, or the old last_pub of now, fails. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <vector>

#include "Arduino.h"
#include "r2s/r2s_session.h"
#include "r2s_arduino_port.h"
#include "support.hpp"

void setup(void);
void loop(void);

static TapStream s_tap;
static size_t s_offset;
static uint8_t s_battery;

static void set_tick(uint32_t ms)
{
    set_stub_micros((unsigned long)ms * 1000UL);
}

/* Boots the sketch at tick ms and handshakes there with the tick frozen. */
static int boot_to_connected(uint32_t ms)
{
    set_tick(ms);
    setup();
    r2s::detail::arduino_port_begin(s_tap, 115200u);
    loop();
    set_tick(ms);
    loop();
    CHECK(handshake(s_tap, loop, "hello_xmacro", s_battery, s_offset) == 0);
    CHECK(r2s_connected() != 0u);
    return 0;
}

static int collect(std::vector<BatterySample> &samples, uint32_t tick)
{
    return collect_battery(s_tap, s_offset, s_battery, tick, samples);
}

/* A slightly late tick publishes once and keeps the phase: the next frame
 * stays on the old grid instead of drifting by the lateness. */
static int phase(void)
{
    std::vector<BatterySample> samples;
    size_t have;

    CHECK(boot_to_connected(10000u) == 0);
    set_tick(10099u);
    loop();
    CHECK(collect(samples, 10099u) == 0);
    CHECK(samples.empty());
    set_tick(10100u);
    loop();
    CHECK(collect(samples, 10100u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(10250u);
    loop();
    CHECK(collect(samples, 10250u) == 0);
    CHECK(samples.size() == 2u);
    set_tick(10299u);
    loop();
    CHECK(collect(samples, 10299u) == 0);
    CHECK(samples.size() == 2u);
    set_tick(10300u);
    loop();
    CHECK(collect(samples, 10300u) == 0);
    CHECK(samples.size() == 3u);
    have = samples.size();
    CHECK(samples[have - 3u].value + 1.0f == samples[have - 2u].value);
    CHECK(samples[have - 2u].value + 1.0f == samples[have - 1u].value);
    CHECK(samples[have - 1u].tick == 10300u);
    return 0;
}

/* A stale deadline publishes once and resyncs to now, never a burst. */
static int stale(void)
{
    std::vector<BatterySample> samples;

    CHECK(boot_to_connected(10000u) == 0);
    set_tick(10100u);
    loop();
    CHECK(collect(samples, 10100u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(10600u);
    loop();
    loop();
    loop();
    CHECK(collect(samples, 10600u) == 0);
    CHECK(samples.size() == 2u);
    set_tick(10699u);
    loop();
    CHECK(collect(samples, 10699u) == 0);
    CHECK(samples.size() == 2u);
    set_tick(10700u);
    loop();
    CHECK(collect(samples, 10700u) == 0);
    CHECK(samples.size() == 3u);
    return 0;
}

/* Going offline resets the deadline: the reconnect publishes nothing until
 * the next period instead of replaying the offline gap. */
static int offline(void)
{
    std::vector<BatterySample> samples;
    size_t reconnect_offset;

    CHECK(boot_to_connected(10000u) == 0);
    set_tick(10100u);
    loop();
    CHECK(collect(samples, 10100u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(25000u);
    loop();
    loop();
    CHECK(collect(samples, 25000u) == 0);
    CHECK(samples.size() == 1u);
    CHECK(r2s_connected() == 0u);
    reconnect_offset = s_offset;
    CHECK(handshake(s_tap, loop, "hello_xmacro", s_battery, s_offset) == 0);
    CHECK(r2s_connected() != 0u);
    {
        /* The reconnect itself publishes nothing: a stale deadline would
         * replay the offline gap in the ready loop. */
        std::vector<TestFrame> frames;
        size_t scan = reconnect_offset;

        CHECK(decode_frames(s_tap.tx(), scan, frames) == 0);
        for (size_t i = 0u; i < frames.size(); i++) {
            CHECK(frames[i].topic != s_battery);
        }
    }
    CHECK(collect(samples, 25000u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(25099u);
    loop();
    CHECK(collect(samples, 25099u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(25100u);
    loop();
    CHECK(collect(samples, 25100u) == 0);
    CHECK(samples.size() == 2u);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: test_xmacro_timing <phase|stale|offline>\n");
        return 2;
    }
    if (strcmp(argv[1], "phase") == 0) {
        return phase();
    }
    if (strcmp(argv[1], "stale") == 0) {
        return stale();
    }
    if (strcmp(argv[1], "offline") == 0) {
        return offline();
    }
    printf("unknown case '%s'\n", argv[1]);
    return 2;
}
