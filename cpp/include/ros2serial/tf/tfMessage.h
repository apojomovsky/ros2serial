/* rosserial spelling of the tf array message: the struct the runtime layer
 * encodes, so no new traits are needed. The wire type keeps its ROS 2 name,
 * which the bridge resolves against the installed tf2_msgs support. */
#ifndef R2S_ROS2SERIAL_TF_TFMESSAGE_H
#define R2S_ROS2SERIAL_TF_TFMESSAGE_H

#include <tf2_msgs/msg/tf_message.hpp>

#include "ros2serial/geometry_msgs/TransformStamped.h"

namespace tf
{
using tfMessage = tf2_msgs::msg::TFMessage;
} // namespace tf

#endif
