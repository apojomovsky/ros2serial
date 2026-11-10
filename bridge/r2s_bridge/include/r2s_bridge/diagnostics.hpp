// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

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
