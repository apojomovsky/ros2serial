// The core's headers from C++: the bridge and the C++ layer include them and
// link the C objects, which needs C linkage on every declaration.
#include "r2s/r2s.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_port.h"

int main()
{
    return r2s_crc16_update(R2S_CRC16_INIT, 0x31) == 0 ? 1 : 0;
}
