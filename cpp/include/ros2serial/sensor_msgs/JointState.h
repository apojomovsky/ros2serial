/* rosserial spelling of the JointState message: the same struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_SENSOR_MSGS_JOINTSTATE_H
#define R2S_ROS2SERIAL_SENSOR_MSGS_JOINTSTATE_H

#include <sensor_msgs/msg/joint_state.hpp>

namespace sensor_msgs
{
/// The JointState message under its rosserial spelling.
using JointState = msg::JointState;
} // namespace sensor_msgs

#endif
