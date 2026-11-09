/* Arduino port glue: the core's port contract over a Stream, default Serial.
 * The sketch never names these; node.begin() selects the stream (V7). */
#include <Arduino.h>

#include "r2s/r2s_port.h"

#include "r2s_arduino_port.h"

namespace
{
// Selected stream, null until attached; the accessors then serve Serial, so
// a sketch that never selects still talks on the default port.
Stream *s_selected;

Stream & selected(void)
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
void arduino_port_attach(Stream & port)
{
  s_selected = &port;
}
} // namespace detail
} // namespace r2s

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  Stream & port = selected();
  uint16_t count = 0u;

  while (count < max && port.available() > 0) {
    buf[count] = (uint8_t)port.read();
    count++;
  }
  return count;
}

#ifdef R2S_TEST_TX_CAP
/* Test-only saturation budget: at most R2S_TEST_TX_CAP bytes leave the port
 * per tick, so a publish past the budget fails with TX_BUSY instead of
 * blocking. Disarmed while the cap holds 0xFFFF, which is the default. */
static uint16_t s_tx_cap = 0xFFFFu;
static uint16_t s_tx_left = 0xFFFFu;
static uint32_t s_tx_tick;

/* Arms the test-only budget: cap bytes per tick, 0xFFFF to disarm back to
 * the blocking report. Declared by test code only, never by the shipped
 * port header, so the public API stays what it was. */
extern "C" void r2s_test_set_tx_cap(uint16_t cap)
{
  s_tx_cap = cap;
  s_tx_left = cap;
  s_tx_tick = r2s_port_tick();
}
#endif

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
#ifdef R2S_TEST_TX_CAP
    /* Spend the budget the last tx_free report promised; the floor keeps
     * a short write from wrapping the count. */
  if (s_tx_left >= len) {
    s_tx_left = (uint16_t)(s_tx_left - len);
  } else {
    s_tx_left = 0u;
  }
#endif
  (void)selected().write(buf, (size_t)len);
}

/* Serial.write blocks for space, so the port never refuses a byte: it reports
 * 0xFFFF, and the Uno ring (16 bytes) never has to hold a whole frame. */
uint16_t r2s_port_tx_free(void)
{
#ifdef R2S_TEST_TX_CAP
  uint32_t now;

  if (s_tx_cap == 0xFFFFu) {
    return 0xFFFFu;
  }
    /* The budget refills when the tick moves, so one tick's flood cannot
     * borrow the next tick's bytes. */
  now = r2s_port_tick();
  if (now != s_tx_tick) {
    s_tx_tick = now;
    s_tx_left = s_tx_cap;
  }
  return s_tx_left;
#else
  return 0xFFFFu;
#endif
}

/* micros() resolves 4 us, fine for the 1 ms tick the core announces. */
uint32_t r2s_port_tick(void)
{
  return (uint32_t)(micros() / 1000UL);
}

#ifdef R2S_TESTING
void r2s_arduino_port_test_reset(void)
{
  s_selected = nullptr;
#ifdef R2S_TEST_TX_CAP
  s_tx_cap = 0xFFFFu;
  s_tx_left = 0xFFFFu;
  s_tx_tick = 0u;
#endif
}
#endif
