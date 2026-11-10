// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* rosserial spelling of the Trigger service: the same request and response
 * the runtime layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_STD_SRVS_TRIGGER_H
#define R2S_ROS2SERIAL_STD_SRVS_TRIGGER_H

#include "std_srvs/srv/trigger.hpp"

namespace std_srvs
{
/// The Trigger service under its rosserial spelling.
using Trigger = srv::Trigger;
} // namespace std_srvs

#endif
