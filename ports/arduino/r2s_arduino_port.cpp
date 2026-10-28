/* Arduino port glue: the core's port contract over a Stream, default Serial.
 * The sketch never names these; node.begin() selects the stream (V7). */
#include <Arduino.h>
#include <EEPROM.h>

#include "r2s/r2s_port.h"

#include "r2s_arduino_port.h"

namespace
{
// Selected stream, null until attached; the accessors then serve Serial, so
// a sketch that never selects still talks on the default port.
Stream *s_selected;
uint32_t s_boot_id;
bool s_boot_done;

Stream &selected(void)
{
    if (s_selected != nullptr) {
        return *s_selected;
    }
    return Serial;
}
} // namespace

namespace r2s
{
namespace detail
{
void arduino_port_attach(Stream &port)
{
    s_selected = &port;
}
} // namespace detail
} // namespace r2s

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    Stream &port = selected();
    uint16_t count = 0u;

    while (count < max && port.available() > 0) {
        buf[count] = (uint8_t)port.read();
        count++;
    }
    return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    (void)selected().write(buf, (size_t)len);
}

/* Serial.write blocks for space, so the port never refuses a byte: it reports
 * 0xFFFF, and the Uno ring (16 bytes) never has to hold a whole frame. */
uint16_t r2s_port_tx_free(void)
{
    return 0xFFFFu;
}

/* micros() resolves 4 us, fine for the 1 ms tick the core announces. */
uint32_t r2s_port_tick(void)
{
    return (uint32_t)(micros() / 1000UL);
}

uint32_t r2s_port_boot_id(void)
{
    if (!s_boot_done) {
        EEPROM.get(0, s_boot_id);
        s_boot_id += 1u;
        EEPROM.put(0, s_boot_id);
        s_boot_done = true;
    }
    return s_boot_id;
}

#ifdef R2S_TESTING
void r2s_arduino_port_test_reset(void)
{
    s_selected = nullptr;
    s_boot_id = 0u;
    s_boot_done = false;
}
#endif
