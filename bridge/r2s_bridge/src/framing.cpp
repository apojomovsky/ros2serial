// The bridge's receive framing: the core's framer reads the transport
// through the installed port and hands back one decoded frame at a time.
#include "r2s_bridge/framing.hpp"

#include <stdexcept>

#include "core_port.hpp"

extern "C" {
#include "r2s/r2s_framer.h"
}

namespace r2s_bridge
{

FrameReader::FrameReader(Transport &transport, const Clock &clock)
{
    if (!attach_core_port(transport, clock)) {
        throw std::logic_error("the core framer keeps one state: one FrameReader per process");
    }
    r2s_framer_init();
}

FrameReader::~FrameReader()
{
    detach_core_port();
}

bool FrameReader::poll(Frame &frame)
{
    r2s_frame_t decoded{};
    if (r2s_framer_poll(&decoded) == 0u) {
        return false;
    }
    frame.data = decoded.data;
    frame.size = decoded.len;
    frame.tick = decoded.tick;
    return true;
}

void FrameReader::release()
{
    r2s_framer_release();
}

} // namespace r2s_bridge
