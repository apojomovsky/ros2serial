// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

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
void arduino_port_attach(Stream & port);

// Starts port at baud and serves it; the baud reaches HardwareSerial and
// USB CDC alike through the concrete type, while the glue keeps a Stream.
template<typename PortT> void arduino_port_begin(PortT & port, unsigned long baud)
{
  port.begin(baud);
  arduino_port_attach(port);
}
}   // namespace detail
} // namespace r2s

#ifdef R2S_TESTING
// Drops the selection and the boot state, so one host binary reboots.
void r2s_arduino_port_test_reset(void);
#endif

#endif
