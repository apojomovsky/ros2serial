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

// Renders a Router::Diagnostics snapshot as the /diagnostics payload: one
// status named like a diagnostic_updater task, the link state as its message
// and every H8 value as a key/value pair. Plain data in, message out, so
// tests cover it without a node; the bridge stamps and publishes the result
// on the same timer that polls the router.
#ifndef R2S_BRIDGE__DIAGNOSTICS_HPP_
#define R2S_BRIDGE__DIAGNOSTICS_HPP_

#include <string>

#include <diagnostic_msgs/msg/diagnostic_array.hpp>

#include "r2s_bridge/router.hpp"

namespace r2s_bridge
{

/// The task part of the status name, in the "<node>: <task>" shape.
/**
 * diagnostic_updater uses this shape. The bridge passes its own node name,
 * so a remapped bridge stays recognizable.
 */
inline const char *kDiagnosticsTaskName = "link";

/// Builds the array for one snapshot.
/**
 * hardware_id names the link, usually the port. The header stamp is left
 * zero for the bridge to fill in.
 *
 * \param[in] snapshot the H8 values to render.
 * \param[in] node_name the bridge node name, for the status name.
 * \param[in] hardware_id the link name, for the status.
 * \return The array holding one status, unstamped.
 */
diagnostic_msgs::msg::DiagnosticArray diagnostics_message(
  const Router::Diagnostics & snapshot,
  const std::string & node_name,
  const std::string & hardware_id);

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__DIAGNOSTICS_HPP_
