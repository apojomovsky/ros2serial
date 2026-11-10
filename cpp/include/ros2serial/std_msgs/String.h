/* rosserial spelling of the String message: the same struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_STD_MSGS_STRING_H
#define R2S_ROS2SERIAL_STD_MSGS_STRING_H

#include <std_msgs/msg/string.hpp>

namespace std_msgs
{
/// The String message under its rosserial spelling.
using String = msg::String;
} // namespace std_msgs

#endif
