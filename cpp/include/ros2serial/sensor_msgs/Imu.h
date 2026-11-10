// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* rosserial spelling of the Imu message: the same struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_SENSOR_MSGS_IMU_H
#define R2S_ROS2SERIAL_SENSOR_MSGS_IMU_H

#include <sensor_msgs/msg/imu.hpp>

namespace sensor_msgs
{
/// The Imu message under its rosserial spelling.
using Imu = msg::Imu;
} // namespace sensor_msgs

#endif
