/* rosserial spelling of the Quaternion message: the same struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_GEOMETRY_MSGS_QUATERNION_H
#define R2S_ROS2SERIAL_GEOMETRY_MSGS_QUATERNION_H

#include <geometry_msgs/msg/quaternion.hpp>

namespace geometry_msgs
{
/// The Quaternion message under its rosserial spelling.
using Quaternion = msg::Quaternion;
} // namespace geometry_msgs

#endif
