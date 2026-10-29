/* The slice firmware owes every missed tick, like the posix firmware it
 * mirrors: a stall is caught up frame by frame, which the message-count
 * scenario needs. Each case drives the real sketch with a frozen tick. The
 * single-frame gate of the examples fails the catch-up case. */
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
    CHECK(handshake(s_tap, loop, "r2s_slice", s_battery, s_offset) == 0);
    CHECK(r2s_connected() != 0u);
    return 0;
}

static int collect(std::vector<BatterySample> &samples, uint32_t tick)
{
    return collect_battery(s_tap, s_offset, s_battery, tick, samples);
}

/* The steady rate is one frame per 20 ms with consecutive values. */
static int rate(void)
{
    std::vector<BatterySample> samples;

    CHECK(boot_to_connected(10000u) == 0);
    set_tick(10019u);
    loop();
    CHECK(collect(samples, 10019u) == 0);
    CHECK(samples.empty());
    set_tick(10020u);
    loop();
    CHECK(collect(samples, 10020u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(10040u);
    loop();
    CHECK(collect(samples, 10040u) == 0);
    CHECK(samples.size() == 2u);
    CHECK(samples[0].value + 1.0f == samples[1].value);
    return 0;
}

/* A stalled loop owes every missed interval: one loop call after a half
 * second stall publishes all 25 frames with consecutive values. */
static int catchup(void)
{
    std::vector<BatterySample> samples;
    size_t i;

    CHECK(boot_to_connected(10000u) == 0);
    set_tick(10020u);
    loop();
    CHECK(collect(samples, 10020u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(10520u);
    loop();
    CHECK(collect(samples, 10520u) == 0);
    CHECK(samples.size() == 26u);
    for (i = 1u; i < samples.size(); i++) {
        CHECK(samples[i - 1u].value + 1.0f == samples[i].value);
    }
    return 0;
}

/* Going offline resets the deadline: the reconnect publishes nothing until
 * the next period instead of bursting the offline gap. */
static int offline(void)
{
    std::vector<BatterySample> samples;
    size_t reconnect_offset;

    CHECK(boot_to_connected(10000u) == 0);
    set_tick(10020u);
    loop();
    CHECK(collect(samples, 10020u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(25000u);
    loop();
    loop();
    CHECK(collect(samples, 25000u) == 0);
    CHECK(samples.size() == 1u);
    CHECK(r2s_connected() == 0u);
    reconnect_offset = s_offset;
    CHECK(handshake(s_tap, loop, "r2s_slice", s_battery, s_offset) == 0);
    CHECK(r2s_connected() != 0u);
    {
        /* The reconnect itself publishes nothing. */
        std::vector<TestFrame> frames;
        size_t scan = reconnect_offset;

        CHECK(decode_frames(s_tap.tx(), scan, frames) == 0);
        for (size_t i = 0u; i < frames.size(); i++) {
            CHECK(frames[i].topic != s_battery);
        }
    }
    CHECK(collect(samples, 25000u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(25019u);
    loop();
    CHECK(collect(samples, 25019u) == 0);
    CHECK(samples.size() == 1u);
    set_tick(25020u);
    loop();
    CHECK(collect(samples, 25020u) == 0);
    CHECK(samples.size() == 2u);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: test_slice_catchup <rate|catchup|offline>\n");
        return 2;
    }
    if (strcmp(argv[1], "rate") == 0) {
        return rate();
    }
    if (strcmp(argv[1], "catchup") == 0) {
        return catchup();
    }
    if (strcmp(argv[1], "offline") == 0) {
        return offline();
    }
    printf("unknown case '%s'\n", argv[1]);
    return 2;
}
