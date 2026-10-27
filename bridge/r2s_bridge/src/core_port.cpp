// The core's port seam for the bridge: the transport and clock one
// FrameReader installed for its framer to read through.
#include "core_port.hpp"

#include <cstdint>

extern "C" {
#include "r2s/r2s_port.h"
}

namespace
{
// The tick is the clock's whole milliseconds, so it wraps every 49 days and
// stays an integer division like the MCU's tick_hz math.
constexpr std::int64_t kNanosecondsPerTick = 1000000;

r2s_bridge::Transport *installed_transport = nullptr;
const r2s_bridge::Clock *installed_clock = nullptr;
bool installed_fault = false;
} // namespace

namespace r2s_bridge
{

bool attach_core_port(Transport &transport, const Clock &clock)
{
    if (installed_transport != nullptr) {
        return false;
    }
    installed_transport = &transport;
    installed_clock = &clock;
    installed_fault = false;
    return true;
}

void detach_core_port()
{
    installed_transport = nullptr;
    installed_clock = nullptr;
    installed_fault = false;
}

bool link_faulted()
{
    return installed_fault;
}

void clear_link_fault()
{
    installed_fault = false;
}

} // namespace r2s_bridge

extern "C" uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    if (installed_transport == nullptr) {
        return 0u;
    }
    const r2s_bridge::IoResult got = installed_transport->read(buf, max);
    if (got.status == r2s_bridge::IoStatus::kOk) {
        return static_cast<uint16_t>(got.bytes);
    }
    if (got.status != r2s_bridge::IoStatus::kWouldBlock) {
        installed_fault = true;
    }
    return 0u;
}

extern "C" uint32_t r2s_port_tick(void)
{
    if (installed_clock == nullptr) {
        return 0u;
    }
    return static_cast<uint32_t>(installed_clock->now().count() / kNanosecondsPerTick);
}
