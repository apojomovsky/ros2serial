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

/// The core's per-link framer state, named without its header.
struct r2s_framer;

namespace r2s_bridge
{

/// One decoded frame, as [FRM-2] lays it out.
/**
 * topic_id, seq, the payload and the CRC, followed by nothing. The bytes
 * stay valid until release() or destruction. tick is the port tick read
 * when the delimiter arrived, one millisecond per tick.
 */
struct Frame
{
  const std::uint8_t *data = nullptr;  ///< The first decoded byte.
  std::uint16_t size = 0;  ///< How many bytes data holds.
  std::uint32_t tick = 0;  ///< The port tick read when the delimiter arrived.
};

/// The bridge's receive framing: one core framer bound to one link.
/**
 * Each reader owns a core framer instance bound to its link, so the host
 * and the MCU share one framing implementation and several links frame
 * in one process. The instance lives behind a pointer so this header
 * stays free of the core's configuration.
 */
class FrameReader
{
public:
  /// Binds an owned core framer instance to transport and clock.
  /**
   * Starts from a clean line.
   *
   * \param[in] transport the link to read through.
   * \param[in] clock the time source for the port ticks.
   * \throws std::logic_error when no link slot is free.
   */
  FrameReader(Transport & transport, const Clock & clock);

  /// Destroys the reader and returns its link slot.
  ~FrameReader();

  /// Not copyable.
  FrameReader(const FrameReader &) = delete;

  /// Not assignable.
  FrameReader & operator=(const FrameReader &) = delete;

  /// Pulls bytes from the link until a frame is decoded or none is left.
  /**
   * Returns true and fills frame when one is ready, false when none is. A
   * frame the core refuses is dropped inside its framer. While an
   * unreleased frame waits, no byte is read, so the next frame stays in
   * the transport.
   *
   * \param[out] frame the decoded frame, on true.
   * \return True when a frame is ready.
   */
  bool poll(Frame & frame);

  /// Releases the frame poll() last returned.
  /**
   * The next poll may start one only after this runs.
   */
  void release();

  /// A read through this reader's link reported kClosed or kError.
  /**
   * True since the last clear_link_fault(). The framer cannot tell an
   * empty link from a dead one, so the session asks here after polling it.
   *
   * \return True when the link faulted.
   */
  bool link_faulted() const;

  /// Forgets the link fault.
  /**
   * The session does this when it opens the link.
   */
  void clear_link_fault();

private:
  std::unique_ptr<struct r2s_framer> framer_;
  std::uint8_t link_ = 0;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__FRAMING_HPP_
