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

// The core's port seam for the bridge: one slot per live link, each reading
// its own transport and clock through the indexed port functions. A
// FrameReader owns the slot its constructor attaches and returns it in its
// destructor, so several readers frame in one process, each on its link.
#ifndef CORE_PORT_HPP_
#define CORE_PORT_HPP_

#include <cstdint>

#include "r2s_bridge/clock.hpp"
#include "r2s_bridge/transport.hpp"

namespace r2s_bridge
{

/// Installs the pair the indexed port functions read through.
/**
 * Returns its link id.
 *
 * \param[in] transport the link to read through.
 * \param[in] clock the time source for the port ticks.
 * \return The slot the pair was installed in.
 * \throws std::logic_error when every slot is taken, naming the link limit.
 */
std::uint8_t attach_core_port(Transport & transport, const Clock & clock);

/// Removes the installed pair, so its slot serves the next link.
/**
 * \param[in] link whose slot to free.
 */
void detach_core_port(std::uint8_t link);

/// A read through the slot's transport reported kClosed or kError.
/**
 * True since the last clear_link_fault(). The core framer cannot tell an
 * empty link from a dead one, so the session asks here after polling it.
 *
 * \param[in] link whose fault to report.
 * \return True when the slot faulted.
 */
bool link_faulted(std::uint8_t link);

/// Forgets the slot's fault, which the session does when it opens a link.
/**
 * \param[in] link whose fault to forget.
 */
void clear_link_fault(std::uint8_t link);

}  // namespace r2s_bridge

#endif  // CORE_PORT_HPP_
