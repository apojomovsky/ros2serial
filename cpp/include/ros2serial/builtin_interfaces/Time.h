/* rosserial spelling of the Time message: the same struct the runtime
 * layer encodes, so no new traits are needed. */
#ifndef R2S_ROS2SERIAL_BUILTIN_INTERFACES_TIME_H
#define R2S_ROS2SERIAL_BUILTIN_INTERFACES_TIME_H

#include <builtin_interfaces/msg/time.hpp>

namespace builtin_interfaces
{
/// The Time message under its rosserial spelling.
using Time = msg::Time;
} // namespace builtin_interfaces

#endif
