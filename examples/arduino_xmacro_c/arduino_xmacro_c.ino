/* C X-macro example: the sketch file is C++, but the topic list lives in
 * the C file next to it. This shim opens the port and forwards setup and
 * loop to the C implementation, which owns the list. */

#include <Arduino.h>

#include "xmacro_c.h"

void setup(void)
{
    Serial.begin(115200);
    xmacro_setup();
}

void loop(void) { xmacro_loop(); }
