// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* rosserial spelling of the Transform message: the struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_GEOMETRY_MSGS_TRANSFORM_H
#define R2S_ROS2SERIAL_GEOMETRY_MSGS_TRANSFORM_H

#include <geometry_msgs/msg/transform.hpp>

namespace geometry_msgs
{
/// The Transform message under its rosserial spelling.
using Transform = msg::Transform;
} // namespace geometry_msgs

#endif
