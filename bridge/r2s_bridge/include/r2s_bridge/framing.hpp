// The bridge's receive framing: the core's framer and CRC read from the
// bridge's transport, so the host and the MCU share one framing
// implementation. The core framer keeps one state per process behind its
// port seam, so one FrameReader is live per process.
#ifndef R2S_BRIDGE__FRAMING_HPP_
#define R2S_BRIDGE__FRAMING_HPP_

#include <cstdint>

#include "r2s_bridge/clock.hpp"
#include "r2s_bridge/transport.hpp"

namespace r2s_bridge
{

// One decoded frame, as [FRM-2] lays it out: topic_id, seq, the payload and
// the CRC, followed by nothing. The bytes stay valid until release() or
// destruction. tick is the port tick read when the delimiter arrived, one
// millisecond per tick.
struct Frame {
    const std::uint8_t *data = nullptr;
    std::uint16_t size = 0;
    std::uint32_t tick = 0;
};

class FrameReader
{
  public:
    // Binds the core framer to transport and clock for this reader's lifetime
    // and starts from a clean line. Throws std::logic_error when another
    // reader is live: the core framer has one state per process.
    FrameReader(Transport &transport, const Clock &clock);
    ~FrameReader();
    FrameReader(const FrameReader &) = delete;
    FrameReader &operator=(const FrameReader &) = delete;

    // Pulls bytes from the transport until a frame is decoded or none is left.
    // Returns true and fills frame when one is ready, false when none is. A
    // frame the core refuses is dropped inside its framer. While an unreleased
    // frame waits, no byte is read, so the next frame stays in the transport.
    bool poll(Frame &frame);

    // Releases the frame poll() last returned, so the next poll may start one.
    void release();
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__FRAMING_HPP_
