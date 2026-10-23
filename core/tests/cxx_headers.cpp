// The core's headers from C++: the bridge and the C++ layer include them and
// link the C objects, which needs C linkage on every declaration.
#include "r2s/r2s.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_rcobs.h"
#include "r2s/r2s_varint.h"

int main()
{
    uint8_t encoded[R2S_VARINT_MAX_BYTES];
    r2s_frame_t frame;
    r2s_rcobs_encoder_t enc;

    if (r2s_crc16_update(R2S_CRC16_INIT, 0x31) == 0) {
        return 1;
    }
    if (r2s_varint_encode(300, encoded) != 2) {
        return 1;
    }
    if (r2s_framer_poll(&frame) != 0 || r2s_framer_counters()->rx_frames != 0) {
        return 1;
    }
    r2s_rcobs_encoder_init(&enc);
    return r2s_rcobs_encode_byte(&enc, 0x11, encoded) == 1 ? 0 : 1;
}
