/* The stub EEPROM behind the glue's host test. */
#include "EEPROM.h"

EepromStub EEPROM;

void eeprom_stub_reset(void)
{
    EEPROM.reset();
}
