// The bridge's receive framing: the core framer reads the link's transport
// through the installed port slot and hands back one decoded frame at a time.
#include "r2s_bridge/framing.hpp"

#include "core_port.hpp"

extern "C" {
#include "r2s/r2s_framer.h"
}

namespace r2s_bridge
{

FrameReader::FrameReader(Transport & transport, const Clock & clock)
: framer_(new r2s_framer_t), link_(attach_core_port(transport, clock))
{
  r2s_framer_init_inst(framer_.get(), link_);
}

FrameReader::~FrameReader()
{
  detach_core_port(link_);
}

bool FrameReader::poll(Frame & frame)
{
  r2s_frame_t decoded{};
  if (r2s_framer_poll_inst(framer_.get(), &decoded) == 0u) {
    return false;
  }
  frame.data = decoded.data;
  frame.size = decoded.len;
  frame.tick = decoded.tick;
  return true;
}

void FrameReader::release()
{
  r2s_framer_release_inst(framer_.get());
}

bool FrameReader::link_faulted() const
{
  return r2s_bridge::link_faulted(link_);
}

void FrameReader::clear_link_fault()
{
  r2s_bridge::clear_link_fault(link_);
}

}  // namespace r2s_bridge
