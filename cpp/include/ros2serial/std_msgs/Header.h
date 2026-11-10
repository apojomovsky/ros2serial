/* rosserial spelling of the Header message: the same struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_STD_MSGS_HEADER_H
#define R2S_ROS2SERIAL_STD_MSGS_HEADER_H

#include <std_msgs/msg/header.hpp>

namespace std_msgs
{
/// The Header message under its rosserial spelling.
using Header = msg::Header;
} // namespace std_msgs

#endif
