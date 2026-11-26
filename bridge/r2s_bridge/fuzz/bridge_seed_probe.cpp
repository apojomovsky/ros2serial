// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// Probe: the bridge seeds must do what they claim before any fuzzing. The
// libFuzzer runs accept a refusal as a legal outcome, so a transcript that
// never reaches the connected state or a converter seed that never
// converts would pass the seeds stage silently; this turns that drift
// into a failure, per transcript and per converter seed. Two transcripts
// legitimately never connect, because their vectors end mid-walk or with
// the topic retries exhausted; they are named below and must keep not
// connecting. It also decodes the hand Int32MultiArray seeds to their
// documented layout, since no message vector carries that type.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/int32_multi_array.hpp>
#include <tf2_msgs/msg/tf_message.hpp>

#include "bridge_session_harness.hpp"
#include "r2s_bridge/converter.hpp"

namespace
{

std::vector<std::uint8_t> read_file(const std::filesystem::path & path)
{
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool starts_with(const std::string & name, const char * prefix)
{
  return name.rfind(prefix, 0) == 0;
}

const char * type_name(std::uint8_t type)
{
  switch (type) {
    case 0:
      return "sensor_msgs/msg/Imu";
    case 1:
      return "sensor_msgs/msg/JointState";
    case 2:
      return "tf2_msgs/msg/TFMessage";
    default:
      return "std_msgs/msg/Int32MultiArray";
  }
}

bool hand_seed_matches(r2s_bridge::Converter & converter, const std::vector<std::uint8_t> & seed)
{
  std_msgs::msg::Int32MultiArray msg;
  const r2s_bridge::ConvertResult result = converter.unpack_to_struct(
    "std_msgs/msg/Int32MultiArray", (seed[1] & 1u) != 0u, seed.data() + 2u, seed.size() - 2u,
    &msg);
  if (result.fail != r2s_bridge::ConvertFail::kNone) {
    return false;
  }
  return msg.layout.dim.size() == 1u && msg.layout.dim[0].label == "x" &&
         msg.layout.dim[0].size == 2u && msg.layout.dim[0].stride == 2u &&
         msg.layout.data_offset == 0u &&
         msg.data == std::vector<std::int32_t>{1, -1};
}

bool unpacks(r2s_bridge::Converter & converter, const std::vector<std::uint8_t> & seed)
{
  const bool narrow = (seed[1] & 1u) != 0u;
  const std::uint8_t * payload = seed.data() + 2u;
  const std::size_t payload_size = seed.size() - 2u;
  sensor_msgs::msg::Imu imu;
  sensor_msgs::msg::JointState joint_state;
  tf2_msgs::msg::TFMessage tf_message;
  std_msgs::msg::Int32MultiArray int32_multi_array;
  switch (seed[0]) {
    case 0:
      return converter.unpack_to_struct(type_name(0), narrow, payload, payload_size, &imu).fail ==
             r2s_bridge::ConvertFail::kNone;
    case 1:
      return converter.unpack_to_struct(
               type_name(1), narrow, payload, payload_size, &joint_state).fail ==
             r2s_bridge::ConvertFail::kNone;
    case 2:
      return converter.unpack_to_struct(
               type_name(2), narrow, payload, payload_size, &tf_message).fail ==
             r2s_bridge::ConvertFail::kNone;
    default:
      return converter.unpack_to_struct(
               type_name(3), narrow, payload, payload_size, &int32_multi_array).fail ==
             r2s_bridge::ConvertFail::kNone;
  }
}

bool never_connects(const std::string & name)
{
  return name == "transcript_service_not_connected" ||
         name == "transcript_topic_retry_exhausted";
}

}  // namespace

int main(int argc, char ** argv)
{
  if (argc != 3) {
    std::fprintf(stderr, "usage: bridge_seed_probe SESSION_DIR CONVERTER_DIR\n");
    return 2;
  }
  int failures = 0;

  std::size_t connected = 0;
  std::size_t transcripts = 0;
  for (const auto & entry : std::filesystem::directory_iterator(argv[1])) {
    const std::string name = entry.path().filename().string();
    if (!starts_with(name, "transcript_")) {
      continue;
    }
    ++transcripts;
    const std::vector<std::uint8_t> seed = read_file(entry.path());
    const bool reaches = r2s_bridge::fuzz::drive_bridge_session(seed.data(), seed.size());
    if (reaches) {
      ++connected;
    }
    if (reaches == never_connects(name)) {
      std::fprintf(stderr, "bridge_seed_probe: %s %s\n", name.c_str(),
        reaches ? "connects but is allowlisted as never connecting" :
        "never reaches connected");
      ++failures;
    }
  }
  std::printf("bridge_seed_probe: %zu of %zu transcripts reach connected\n", connected,
    transcripts);
  if (transcripts == 0u) {
    std::fprintf(stderr, "bridge_seed_probe: no transcripts found\n");
    ++failures;
  }

  r2s_bridge::Converter converter;
  bool converted[4] = {false, false, false, false};
  for (const auto & entry : std::filesystem::directory_iterator(argv[2])) {
    const std::string name = entry.path().filename().string();
    if (!starts_with(name, "vector_") && !starts_with(name, "hand_")) {
      continue;
    }
    const std::vector<std::uint8_t> seed = read_file(entry.path());
    if (seed.size() < 2u || seed[0] > 3u) {
      std::fprintf(stderr, "bridge_seed_probe: %s has a bad head\n", name.c_str());
      ++failures;
      continue;
    }
    std::vector<std::uint8_t> cdr;
    const r2s_bridge::ConvertResult result = converter.to_cdr(
      type_name(seed[0]), (seed[1] & 1u) != 0u, seed.data() + 2u, seed.size() - 2u, cdr);
    if (result.fail == r2s_bridge::ConvertFail::kNone) {
      converted[seed[0]] = true;
    } else {
      std::fprintf(stderr, "bridge_seed_probe: %s does not convert to CDR\n", name.c_str());
      ++failures;
    }
    if (!unpacks(converter, seed)) {
      std::fprintf(stderr, "bridge_seed_probe: %s does not unpack into its struct\n",
        name.c_str());
      ++failures;
    }
    if (starts_with(name, "hand_") && !hand_seed_matches(converter, seed)) {
      std::fprintf(stderr, "bridge_seed_probe: %s does not decode to its layout\n",
        name.c_str());
      ++failures;
    }
  }
  for (std::uint8_t type = 0; type < 4u; ++type) {
    if (!converted[type]) {
      std::fprintf(stderr, "bridge_seed_probe: no seed converts for %s\n", type_name(type));
      ++failures;
    }
  }
  return failures == 0 ? 0 : 1;
}
