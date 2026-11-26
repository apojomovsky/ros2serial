// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// Fuzz target: the bridge converter on arbitrary packed payloads. The
// first byte picks the message type, the second the narrow flag, the rest
// is the payload; both unpack_to_struct and to_cdr run over it. The
// converter must accept or refuse every input without crashing, hanging
// or touching memory past the message; the sanitizers are the oracle.
#include <cstddef>
#include <cstdint>
#include <vector>

#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/int32_multi_array.hpp>
#include <tf2_msgs/msg/tf_message.hpp>

#include "r2s_bridge/converter.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data,
                                      std::size_t size) {
  using namespace r2s_bridge;

  if (size < 2u) {
    return 0;
  }
  const std::uint8_t type = static_cast<std::uint8_t>(data[0] % 4u);
  const bool narrow = (data[1] & 1u) != 0u;
  const std::uint8_t *payload = data + 2u;
  const std::size_t payload_size = size - 2u;

  Converter converter;
  std::vector<std::uint8_t> cdr;
  sensor_msgs::msg::Imu imu;
  sensor_msgs::msg::JointState joint_state;
  tf2_msgs::msg::TFMessage tf_message;
  std_msgs::msg::Int32MultiArray int32_multi_array;

  switch (type) {
  case 0:
    converter.to_cdr("sensor_msgs/msg/Imu", narrow, payload, payload_size, cdr);
    converter.unpack_to_struct("sensor_msgs/msg/Imu", narrow, payload,
                               payload_size, &imu);
    break;
  case 1:
    converter.to_cdr("sensor_msgs/msg/JointState", narrow, payload,
                     payload_size, cdr);
    converter.unpack_to_struct("sensor_msgs/msg/JointState", narrow, payload,
                               payload_size, &joint_state);
    break;
  case 2:
    converter.to_cdr("tf2_msgs/msg/TFMessage", narrow, payload, payload_size,
                     cdr);
    converter.unpack_to_struct("tf2_msgs/msg/TFMessage", narrow, payload,
                               payload_size, &tf_message);
    break;
  default:
    converter.to_cdr("std_msgs/msg/Int32MultiArray", narrow, payload,
                     payload_size, cdr);
    converter.unpack_to_struct("std_msgs/msg/Int32MultiArray", narrow, payload,
                               payload_size, &int32_multi_array);
    break;
  }
  return 0;
}
