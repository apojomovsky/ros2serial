/* Arduino port selection: which stream the glue serves, default Serial.
 * node.begin() calls begin() (V7); until then the sketch begins Serial
 * itself and the glue serves it. Stays internal: no sketch names it. */
#ifndef R2S_ARDUINO_PORT_H
#define R2S_ARDUINO_PORT_H

#include <Arduino.h>

namespace r2s
{
namespace detail
{
// Serves port from now on; the glue defaults to Serial without this call.
void arduino_port_attach(Stream &port);

// Starts port at baud and serves it; the baud reaches HardwareSerial and
// USB CDC alike through the concrete type, while the glue keeps a Stream.
template <typename PortT> void arduino_port_begin(PortT &port, unsigned long baud)
{
    port.begin(baud);
    arduino_port_attach(port);
}
} // namespace detail
} // namespace r2s

#ifdef R2S_TESTING
// Drops the selection and the boot state, so one host binary reboots.
void r2s_arduino_port_test_reset(void);
#endif

#endif
