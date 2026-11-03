/* rosserial spelling of the Imu message: the same struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_SENSOR_MSGS_IMU_H
#define R2S_ROS2SERIAL_SENSOR_MSGS_IMU_H

#include <sensor_msgs/msg/imu.hpp>

namespace sensor_msgs
{
using Imu = msg::Imu;
} // namespace sensor_msgs

#endif
