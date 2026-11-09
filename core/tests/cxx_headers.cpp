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

// The core's headers from C++: the bridge and the C++ layer include them and
// link the C objects, which needs C linkage on every declaration.
#include "r2s/r2s.h"
#include "r2s/r2s_arch.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_crc.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_rcobs.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"
#include "r2s/r2s_tx.h"
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
    if (r2s_tx_wire_bound(0) != 6) {
        return 1;
    }
    r2s_rcobs_encoder_init(&enc);
    return r2s_rcobs_encode_byte(&enc, 0x11, encoded) == 1 ? 0 : 1;
}
