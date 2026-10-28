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

// The status name the bridge publishes under, in the "<node>: <task>" shape
// diagnostic_updater uses.
inline const char *kDiagnosticsStatusName = "r2s_bridge: link";

// The key for the time sync values until the P3 estimator lands.
inline const char *kDiagnosticsTimeSyncKey = "time sync";

// Builds the array for one snapshot. hardware_id names the link, usually the
// port. The header stamp is left zero for the bridge to fill in.
diagnostic_msgs::msg::DiagnosticArray diagnostics_message(const Router::Diagnostics &snapshot,
                                                          const std::string &hardware_id);

} // namespace r2s_bridge

#endif // R2S_BRIDGE__DIAGNOSTICS_HPP_
