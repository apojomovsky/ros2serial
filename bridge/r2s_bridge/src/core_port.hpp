// The core's port seam for the bridge: one slot per live link, each reading
// its own transport and clock through the indexed port functions. A
// FrameReader owns the slot its constructor attaches and returns it in its
// destructor, so several readers frame in one process, each on its link.
#ifndef R2S_BRIDGE__CORE_PORT_HPP_
#define R2S_BRIDGE__CORE_PORT_HPP_

#include <cstdint>

#include "r2s_bridge/clock.hpp"
#include "r2s_bridge/transport.hpp"

namespace r2s_bridge
{

// Installs the pair the indexed port functions read through and returns its
// link id. Throws std::logic_error when every slot is taken, naming the
// link limit.
std::uint8_t attach_core_port(Transport &transport, const Clock &clock);

// Removes the installed pair, so its slot serves the next link.
void detach_core_port(std::uint8_t link);

// True when a read through the slot's transport reported kClosed or kError
// since the last clear_link_fault(). The core framer cannot tell an empty
// link from a dead one, so the session asks here after polling it.
bool link_faulted(std::uint8_t link);

// Forgets the slot's fault, which the session does when it opens a link.
void clear_link_fault(std::uint8_t link);

} // namespace r2s_bridge

#endif // R2S_BRIDGE__CORE_PORT_HPP_
