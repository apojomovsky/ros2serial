// The core's port seam for the bridge: reads and ticks come from the one
// transport and clock the live FrameReader installed. One pair at a time,
// because the core framer keeps one state per process.
#ifndef R2S_BRIDGE__CORE_PORT_HPP_
#define R2S_BRIDGE__CORE_PORT_HPP_

#include "r2s_bridge/clock.hpp"
#include "r2s_bridge/transport.hpp"

namespace r2s_bridge
{

// Installs the pair the port functions read through. Returns false when a
// pair is already installed.
bool attach_core_port(Transport &transport, const Clock &clock);

// Removes the installed pair, if any.
void detach_core_port();

} // namespace r2s_bridge

#endif // R2S_BRIDGE__CORE_PORT_HPP_
