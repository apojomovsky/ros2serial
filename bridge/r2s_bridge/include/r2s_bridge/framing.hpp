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

// The bridge's receive framing: each reader owns a core framer instance
// bound to its link, so the host and the MCU share one framing
// implementation and several links frame in one process. The instance lives
// behind a pointer so this header stays free of the core's configuration.
#ifndef R2S_BRIDGE__FRAMING_HPP_
#define R2S_BRIDGE__FRAMING_HPP_

#include <cstdint>
#include <memory>

#include "r2s_bridge/clock.hpp"
#include "r2s_bridge/transport.hpp"

// The core's per-link framer state, named without its header.
struct r2s_framer;

namespace r2s_bridge
{

// One decoded frame, as [FRM-2] lays it out: topic_id, seq, the payload and
// the CRC, followed by nothing. The bytes stay valid until release() or
// destruction. tick is the port tick read when the delimiter arrived, one
// millisecond per tick.
struct Frame
{
  const std::uint8_t *data = nullptr;
  std::uint16_t size = 0;
  std::uint32_t tick = 0;
};

class FrameReader
{
public:
    // Binds an owned core framer instance to transport and clock and starts
    // from a clean line. Throws std::logic_error when no link slot is free.
  FrameReader(Transport & transport, const Clock & clock);
  ~FrameReader();
  FrameReader(const FrameReader &) = delete;
  FrameReader & operator=(const FrameReader &) = delete;

    // Pulls bytes from the link until a frame is decoded or none is left.
    // Returns true and fills frame when one is ready, false when none is. A
    // frame the core refuses is dropped inside its framer. While an unreleased
    // frame waits, no byte is read, so the next frame stays in the transport.
  bool poll(Frame & frame);

    // Releases the frame poll() last returned, so the next poll may start one.
  void release();

    // True when a read through this reader's link reported kClosed or kError
    // since the last clear_link_fault(). The framer cannot tell an empty
    // link from a dead one, so the session asks here after polling it.
  bool link_faulted() const;

    // Forgets the link fault, which the session does when it opens the link.
  void clear_link_fault();

private:
  std::unique_ptr<struct r2s_framer> framer_;
  std::uint8_t link_ = 0;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__FRAMING_HPP_
