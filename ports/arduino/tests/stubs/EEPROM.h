/* The stub EEPROM the glue's host test compiles against: a byte array
 * behind the get/put templates, cleared on reset like a fresh part. */
#ifndef EEPROM_H
#define EEPROM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

void eeprom_stub_reset(void);

class EepromStub
{
  public:
    template <typename T> T &get(int address, T &value)
    {
        memcpy(&value, &m_cells[address], sizeof(T));
        return value;
    }

    template <typename T> void put(int address, const T &value)
    {
        memcpy(&m_cells[address], &value, sizeof(T));
    }

    void reset(void)
    {
        memset(m_cells, 0, sizeof m_cells);
    }

  private:
    uint8_t m_cells[64];
};

extern EepromStub EEPROM;

#endif
