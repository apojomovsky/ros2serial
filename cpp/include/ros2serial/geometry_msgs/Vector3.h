// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* rosserial spelling of the Vector3 message: the same struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_GEOMETRY_MSGS_VECTOR3_H
#define R2S_ROS2SERIAL_GEOMETRY_MSGS_VECTOR3_H

#include <geometry_msgs/msg/vector3.hpp>

namespace geometry_msgs
{
/// The Vector3 message under its rosserial spelling.
using Vector3 = msg::Vector3;
} // namespace geometry_msgs

#endif
