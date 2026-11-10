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
