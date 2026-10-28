/* The Arduino glue's host test: the port contract against stub streams.
 * Fails unless the glue reads the selected stream, writes to it, reports
 * blocking TX space and ticks from micros. */
#include <stdint.h>
#include <stdio.h>

#include "Arduino.h"
#include "r2s/r2s_port.h"
#include "r2s_arduino_port.h"

#define CHECK(cond)                                                                                          \
    do {                                                                                                     \
        if (!(cond)) {                                                                                       \
            printf("fail line %d: %s\n", __LINE__, #cond);                                                   \
            return 1;                                                                                        \
        }                                                                                                    \
    } while (0)

static void reset_all(void)
{
    Serial = StubStream();
    Serial1 = StubStream();
    set_stub_micros(0);
    r2s_arduino_port_test_reset();
}

static int check_default_port_is_serial(void)
{
    const uint8_t hello[] = {'h', 'i'};
    uint8_t buf[8];
    uint16_t got;

    reset_all();
    Serial.push(hello, sizeof hello);
    got = r2s_port_read(buf, sizeof buf);
    CHECK(got == 2u);
    CHECK(buf[0] == (uint8_t)'h' && buf[1] == (uint8_t)'i');
    return 0;
}

static int check_read_stops_at_empty(void)
{
    const uint8_t one[] = {'a'};
    uint8_t buf[8];
    uint16_t got;

    reset_all();
    Serial.push(one, sizeof one);
    got = r2s_port_read(buf, sizeof buf);
    CHECK(got == 1u);
    CHECK(buf[0] == (uint8_t)'a');
    CHECK(r2s_port_read(buf, sizeof buf) == 0u);
    CHECK(r2s_port_read(buf, 0u) == 0u);
    return 0;
}

static int check_write_goes_to_serial(void)
{
    const uint8_t out[] = {'o', 'k'};
    uint8_t buf[8];
    uint16_t got;

    reset_all();
    r2s_port_write(out, sizeof out);
    got = r2s_port_read(buf, sizeof buf);
    CHECK(got == 2u);
    CHECK(buf[0] == (uint8_t)'o' && buf[1] == (uint8_t)'k');
    return 0;
}

/* Serial.write blocks for space, so the glue reports 0xFFFF even when the
 * ring is nearly full: a whole-frame reservation must never refuse. */
static int check_tx_free_reports_blocking(void)
{
    reset_all();
    Serial.tx_space = 3;
    CHECK(r2s_port_tx_free() == 0xFFFFu);
    Serial.tx_space = 0;
    CHECK(r2s_port_tx_free() == 0xFFFFu);
    return 0;
}

static int check_tick_is_micros_in_milliseconds(void)
{
    reset_all();
    set_stub_micros(1500UL);
    CHECK(r2s_port_tick() == 1u);
    set_stub_micros(4000000000UL);
    CHECK(r2s_port_tick() == 4000000u);
    return 0;
}

static int check_another_port_is_selectable_without_port_code(void)
{
    const uint8_t one[] = {'z'};
    uint8_t buf[8];

    reset_all();
    r2s::detail::arduino_port_begin(Serial1, 115200u);
    Serial.push(one, sizeof one);
    CHECK(r2s_port_read(buf, sizeof buf) == 0u);
    Serial1.push(one, sizeof one);
    CHECK(r2s_port_read(buf, sizeof buf) == 1u);
    CHECK(buf[0] == (uint8_t)'z');
    return 0;
}

int main(void)
{
    CHECK(check_default_port_is_serial() == 0);
    CHECK(check_read_stops_at_empty() == 0);
    CHECK(check_write_goes_to_serial() == 0);
    CHECK(check_tx_free_reports_blocking() == 0);
    CHECK(check_tick_is_micros_in_milliseconds() == 0);
    CHECK(check_another_port_is_selectable_without_port_code() == 0);
    printf("arduino port ok\n");
    return 0;
}
