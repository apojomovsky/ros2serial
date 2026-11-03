/* rosserial spelling of the Twist message: the same struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_GEOMETRY_MSGS_TWIST_H
#define R2S_ROS2SERIAL_GEOMETRY_MSGS_TWIST_H

#include <geometry_msgs/msg/twist.hpp>

namespace geometry_msgs
{
using Twist = msg::Twist;
} // namespace geometry_msgs

#endif
