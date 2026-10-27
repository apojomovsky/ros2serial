// The packed to CDR converter against rclcpp serialization: every message
// vector converts byte-identically, the plan caches per type, and the field
// index order matches the MCU codec's report order.
// covers: ENC-1, ENC-2, ENC-3, ENC-4, ENC-5, ENC-6, ENC-7, ENC-8, ENC-9
// covers: ENC-10, ENC-11, ENC-14, ENC-17, ENC-18
// covers: VAR-1, VAR-2, VAR-3, VAR-4
#include <gtest/gtest.h>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <rclcpp/serialization.hpp>
#include <rclcpp/serialized_message.hpp>

#include <geometry_msgs/msg/pose_array.hpp>
#include <geometry_msgs/msg/twist.hpp>
#include <sensor_msgs/msg/imu.hpp>
#include <sensor_msgs/msg/joint_state.hpp>
#include <std_msgs/msg/bool.hpp>
#include <std_msgs/msg/byte.hpp>
#include <std_msgs/msg/char.hpp>
#include <std_msgs/msg/float32.hpp>
#include <std_msgs/msg/float64.hpp>
#include <std_msgs/msg/int16.hpp>
#include <std_msgs/msg/int32.hpp>
#include <std_msgs/msg/int64.hpp>
#include <std_msgs/msg/int8.hpp>
#include <std_msgs/msg/string.hpp>
#include <std_msgs/msg/u_int16.hpp>
#include <std_msgs/msg/u_int32.hpp>
#include <std_msgs/msg/u_int64.hpp>
#include <std_msgs/msg/u_int8.hpp>

#include "r2s_bridge/converter.hpp"
#include "rosidl_typesupport_introspection_cpp/field_types.hpp"
#include "rosidl_typesupport_introspection_cpp/message_introspection.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

std::vector<std::uint8_t> bytes_of(const std::string &hex)
{
    std::vector<std::uint8_t> bytes;
    std::istringstream in(hex);
    std::string pair;
    while (in >> pair) {
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(pair, nullptr, 16)));
    }
    return bytes;
}

std::string hex_of(const std::vector<std::uint8_t> &bytes)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (i != 0) {
            out << ' ';
        }
        out << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return out.str();
}

// Little-endian bytes of an integer value, whatever the host order is.
std::vector<std::uint8_t> le_u64(std::uint64_t value, std::size_t count)
{
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < count; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    return out;
}

std::vector<std::uint8_t> le_double(double value)
{
    std::uint64_t raw = 0;
    std::memcpy(&raw, &value, sizeof raw);
    return le_u64(raw, 8);
}

std::vector<std::uint8_t> le_float(float value)
{
    std::uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof raw);
    return le_u64(raw, 4);
}

template <typename T> std::vector<std::uint8_t> serialize_ref(const T &msg)
{
    // The middleware leaves CDR alignment padding unwritten; zeroing an
    // oversized buffer first makes the reference bytes deterministic, the
    // same way the converter does.
    rclcpp::Serialization<T> serializer;
    rclcpp::SerializedMessage serialized(4096);
    rcl_serialized_message_t &rcl = serialized.get_rcl_serialized_message();
    std::memset(rcl.buffer, 0, rcl.buffer_capacity);
    serializer.serialize_message(&msg, &serialized);
    return {rcl.buffer, rcl.buffer + rcl.buffer_length};
}

// The host side of [ENC-11]: binary64 through binary32 with roundTiesToEven,
// carried out by the hardware conversion.
double narrow_to_host(double value)
{
    return static_cast<double>(static_cast<float>(value));
}

double yaml_double(const YAML::Node &node, bool narrow)
{
    double value = 0;
    if (node.IsMap()) {
        std::uint64_t raw = std::stoull(node["bits"].as<std::string>(), nullptr, 16);
        std::memcpy(&value, &raw, sizeof value);
        return value;
    }
    value = node.as<double>();
    return narrow ? narrow_to_host(value) : value;
}

void expect_cdr(Converter &converter, const std::string &type, bool narrow,
                const std::vector<std::uint8_t> &payload, const std::vector<std::uint8_t> &want,
                const std::string &trace)
{
    SCOPED_TRACE(trace);
    std::vector<std::uint8_t> cdr;
    const ConvertResult result = converter.to_cdr(type, narrow, payload.data(), payload.size(), cdr);
    ASSERT_EQ(result.fail, ConvertFail::kNone) << "field " << static_cast<int>(result.field);
    if (cdr != want) {
        ADD_FAILURE() << "CDR differs\n got: " << hex_of(cdr) << "\nwant: " << hex_of(want);
    }
}

} // namespace

TEST(ConverterVectors, MatchesRclcppSerializationForEveryVector)
{
    Converter converter;
    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(R2S_MESSAGES_VECTOR_DIR)) {
        if (entry.path().extension() == ".yaml") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    ASSERT_FALSE(files.empty()) << "no message vectors found";

    for (const auto &file : files) {
        const YAML::Node node = YAML::LoadFile(file);
        const std::string name = node["name"].as<std::string>();
        const std::string type = node["type"].as<std::string>();
        const bool narrow = node["profile"]["narrow_float64"].as<bool>();
        const std::vector<std::uint8_t> payload = bytes_of(node["payload"].as<std::string>());
        SCOPED_TRACE(name);
        if (type == "std_msgs/msg/Float32") {
            std_msgs::msg::Float32 msg;
            const YAML::Node data = node["message"]["data"];
            ASSERT_FALSE(narrow) << "no narrow Float32 vector exists";
            if (data.IsMap()) {
                std::uint32_t raw =
                    static_cast<std::uint32_t>(std::stoul(data["bits"].as<std::string>(), nullptr, 16));
                std::memcpy(&msg.data, &raw, sizeof raw);
            } else {
                msg.data = data.as<float>();
            }
            expect_cdr(converter, type, narrow, payload, serialize_ref(msg), name);
        } else if (type == "geometry_msgs/msg/Twist") {
            geometry_msgs::msg::Twist msg;
            msg.linear.x = yaml_double(node["message"]["linear"]["x"], narrow);
            msg.linear.y = yaml_double(node["message"]["linear"]["y"], narrow);
            msg.linear.z = yaml_double(node["message"]["linear"]["z"], narrow);
            msg.angular.x = yaml_double(node["message"]["angular"]["x"], narrow);
            msg.angular.y = yaml_double(node["message"]["angular"]["y"], narrow);
            msg.angular.z = yaml_double(node["message"]["angular"]["z"], narrow);
            expect_cdr(converter, type, narrow, payload, serialize_ref(msg), name);
        } else {
            FAIL() << "no reference for vector type " << type;
        }
    }
}

TEST(ConverterCache, BuildsOnePlanPerType)
{
    Converter converter;
    const std::vector<std::uint8_t> payload{0x00, 0x00, 0xC0, 0x3F};
    std::vector<std::uint8_t> cdr;
    EXPECT_EQ(converter.to_cdr("std_msgs/msg/Float32", false, payload.data(), payload.size(), cdr).fail,
              ConvertFail::kNone);
    EXPECT_EQ(converter.to_cdr("std_msgs/msg/Float32", false, payload.data(), payload.size(), cdr).fail,
              ConvertFail::kNone);
    const std::vector<std::uint8_t> twist(48, 0);
    EXPECT_EQ(converter.to_cdr("geometry_msgs/msg/Twist", false, twist.data(), twist.size(), cdr).fail,
              ConvertFail::kNone);
    EXPECT_EQ(converter.cached_plan_count(), 2u);
}

TEST(ConverterErrors, ReportsTheDepthFirstFieldIndex)
{
    Converter converter;
    const std::vector<std::uint8_t> twist(48, 0);
    const std::vector<std::uint8_t> narrow_twist(24, 0);
    std::vector<std::uint8_t> cdr;

    auto convert = [&](const std::string &type, bool narrow, const std::vector<std::uint8_t> &payload) {
        return converter.to_cdr(type, narrow, payload.data(), payload.size(), cdr);
    };

    // Twist counts the nested field itself, then its children: linear is 0,
    // linear.x is 1, angular is 4, angular.z is 7.
    ConvertResult result = convert("geometry_msgs/msg/Twist", false,
                                   std::vector<std::uint8_t>(twist.begin(), twist.begin() + 47));
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 7u);

    result = convert("geometry_msgs/msg/Twist", true,
                     std::vector<std::uint8_t>(narrow_twist.begin(), narrow_twist.begin() + 3));
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 1u);

    std::vector<std::uint8_t> trailing(49, 0);
    result = convert("geometry_msgs/msg/Twist", false, trailing);
    EXPECT_EQ(result.fail, ConvertFail::kTrailing);
    EXPECT_EQ(result.field, 0xFFu);

    result = convert("std_msgs/msg/Float32", false, {});
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 0u);

    result = convert("no_such_pkg/msg/Nope", false, twist);
    EXPECT_EQ(result.fail, ConvertFail::kUnknownType);
    EXPECT_EQ(result.field, 0xFFu);

    result = convert("Float32", false, twist);
    EXPECT_EQ(result.fail, ConvertFail::kUnknownType);
    EXPECT_EQ(result.field, 0xFFu);
}

TEST(ConverterFieldPath, NamesTheOffendingField)
{
    Converter converter;
    EXPECT_EQ(converter.field_path("std_msgs/msg/Float32", 0), "data");
    EXPECT_EQ(converter.field_path("std_msgs/msg/Float32", 1), "");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/Twist", 0), "linear");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/Twist", 1), "linear.x");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/Twist", 3), "linear.z");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/Twist", 4), "angular");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/Twist", 5), "angular.x");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/Twist", 7), "angular.z");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/Twist", 8), "");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 0), "header");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 1), "header.stamp");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 2), "header.stamp.sec");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 3), "header.stamp.nanosec");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 4), "header.frame_id");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 5), "orientation");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 6), "orientation.x");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 9), "orientation.w");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 10), "orientation_covariance");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 11), "angular_velocity");
    EXPECT_EQ(converter.field_path("sensor_msgs/msg/Imu", 20), "linear_acceleration_covariance");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 0), "header");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 1), "header.stamp");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 4), "header.frame_id");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 5), "poses");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 6), "poses.position");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 7), "poses.position.x");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 10), "poses.orientation");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 14), "poses.orientation.w");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 15), "poses.position");
    EXPECT_EQ(converter.field_path("geometry_msgs/msg/PoseArray", 23), "poses.orientation.w");
    EXPECT_EQ(converter.field_path("no_such_pkg/msg/Nope", 0), "");
}
TEST(ConverterScalars, ConvertsEveryIntegerWidth)
{
    Converter converter;

    const auto check = [&](const std::string &type, const std::vector<std::uint8_t> &payload,
                           const std::vector<std::uint8_t> &want) {
        expect_cdr(converter, type, false, payload, want, type);
    };

    std_msgs::msg::Int8 int8;
    int8.data = -5;
    check("std_msgs/msg/Int8", le_u64(static_cast<std::uint64_t>(0xFB), 1), serialize_ref(int8));

    std_msgs::msg::UInt8 uint8;
    uint8.data = 250;
    check("std_msgs/msg/UInt8", le_u64(250, 1), serialize_ref(uint8));

    std_msgs::msg::Int16 int16;
    int16.data = -300;
    check("std_msgs/msg/Int16", le_u64(static_cast<std::uint64_t>(static_cast<std::uint16_t>(-300)), 2),
          serialize_ref(int16));

    std_msgs::msg::UInt16 uint16;
    uint16.data = 60000;
    check("std_msgs/msg/UInt16", le_u64(60000, 2), serialize_ref(uint16));

    std_msgs::msg::Int32 int32;
    int32.data = -70000;
    check("std_msgs/msg/Int32", le_u64(static_cast<std::uint64_t>(static_cast<std::uint32_t>(-70000)), 4),
          serialize_ref(int32));

    std_msgs::msg::UInt32 uint32;
    uint32.data = 3000000000u;
    check("std_msgs/msg/UInt32", le_u64(3000000000u, 4), serialize_ref(uint32));

    std_msgs::msg::Int64 int64;
    int64.data = -1;
    check("std_msgs/msg/Int64", le_u64(0xFFFFFFFFFFFFFFFFu, 8), serialize_ref(int64));

    std_msgs::msg::UInt64 uint64;
    uint64.data = 0xFFFFFFFFFFFFFFFFu;
    check("std_msgs/msg/UInt64", le_u64(0xFFFFFFFFFFFFFFFFu, 8), serialize_ref(uint64));

    std_msgs::msg::Byte byte;
    byte.data = 0xABu;
    check("std_msgs/msg/Byte", le_u64(0xAB, 1), serialize_ref(byte));

    std_msgs::msg::Char ch;
    ch.data = 65;
    check("std_msgs/msg/Char", le_u64(65, 1), serialize_ref(ch));

    std_msgs::msg::Bool boolean;
    boolean.data = true;
    check("std_msgs/msg/Bool", {0x01}, serialize_ref(boolean));

    std_msgs::msg::Float64 float64;
    float64.data = 1.1;
    check("std_msgs/msg/Float64", le_double(1.1), serialize_ref(float64));

    std_msgs::msg::Float64 narrow;
    narrow.data = narrow_to_host(1.5);
    expect_cdr(converter, "std_msgs/msg/Float64", true, le_float(1.5F), serialize_ref(narrow),
               "narrow Float64");

    std::vector<std::uint8_t> cdr;
    ConvertResult result =
        converter.to_cdr("std_msgs/msg/Bool", false, std::vector<std::uint8_t>{0x02}.data(), 1, cdr);
    EXPECT_EQ(result.fail, ConvertFail::kBadBool);
    EXPECT_EQ(result.field, 0u);

    result = converter.to_cdr("std_msgs/msg/Bool", false, cdr.data(), 0, cdr);
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 0u);

    result = converter.to_cdr("std_msgs/msg/Int16", false, std::vector<std::uint8_t>{0x12}.data(), 1, cdr);
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 0u);

    result = converter.to_cdr("std_msgs/msg/Int8", false, cdr.data(), 0, cdr);
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 0u);

    result = converter.to_cdr("std_msgs/msg/Int32", false, std::vector<std::uint8_t>{0x01, 0x02, 0x03}.data(),
                              3, cdr);
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 0u);

    result =
        converter.to_cdr("std_msgs/msg/Int64", false,
                         std::vector<std::uint8_t>{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}.data(), 7, cdr);
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 0u);
}

TEST(ConverterStrings, ConvertsLengthsAndRejectsBadOnes)
{
    Converter converter;

    std_msgs::msg::String hi;
    hi.data = "hi";
    expect_cdr(converter, "std_msgs/msg/String", false, {0x02, 'h', 'i'}, serialize_ref(hi), "hi");

    std_msgs::msg::String empty;
    expect_cdr(converter, "std_msgs/msg/String", false, {0x00}, serialize_ref(empty), "empty");

    const std::string long_text(130, 'a');
    std_msgs::msg::String long_msg;
    long_msg.data = long_text;
    std::vector<std::uint8_t> long_payload{0x82, 0x01};
    long_payload.insert(long_payload.end(), long_text.begin(), long_text.end());
    expect_cdr(converter, "std_msgs/msg/String", false, long_payload, serialize_ref(long_msg), "long");

    std::vector<std::uint8_t> cdr;
    const auto convert = [&](const std::vector<std::uint8_t> &payload) {
        return converter.to_cdr("std_msgs/msg/String", false, payload.data(), payload.size(), cdr);
    };

    ConvertResult result = convert({0x01, 0x00});
    EXPECT_EQ(result.fail, ConvertFail::kNulInString);
    EXPECT_EQ(result.field, 0u);

    result = convert({0x82});
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 0u);

    result = convert({0x82, 0x80});
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 0u);

    result = convert({0x80, 0x80, 0x80});
    EXPECT_EQ(result.fail, ConvertFail::kBadVarint);
    EXPECT_EQ(result.field, 0u);

    result = convert({0x80, 0x00});
    EXPECT_EQ(result.fail, ConvertFail::kBadVarint);
    EXPECT_EQ(result.field, 0u);

    result = convert({0x80, 0x80, 0x00});
    EXPECT_EQ(result.fail, ConvertFail::kBadVarint);
    EXPECT_EQ(result.field, 0u);

    result = convert({0x80, 0x80, 0x04});
    EXPECT_EQ(result.fail, ConvertFail::kBadVarint);
    EXPECT_EQ(result.field, 0u);

    result = convert({0x02, 'h'});
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 0u);
}

TEST(ConverterSequences, ConvertsJointState)
{
    Converter converter;
    sensor_msgs::msg::JointState msg;
    msg.name = {"a", "bb"};
    msg.position = {1.5, -2.0};

    std::vector<std::uint8_t> payload(9, 0x00);
    payload.insert(payload.end(), {0x02, 0x01, 'a', 0x02, 'b', 'b', 0x02});
    const std::vector<std::uint8_t> one_half = le_double(1.5);
    const std::vector<std::uint8_t> minus_two = le_double(-2.0);
    payload.insert(payload.end(), one_half.begin(), one_half.end());
    payload.insert(payload.end(), minus_two.begin(), minus_two.end());
    payload.push_back(0x00);
    payload.push_back(0x00);
    expect_cdr(converter, "sensor_msgs/msg/JointState", false, payload, serialize_ref(msg), "joint state");
}

TEST(ConverterSequences, ReportsSequenceFieldIndices)
{
    Converter converter;
    std::vector<std::uint8_t> cdr;
    const auto convert = [&](const std::vector<std::uint8_t> &payload) {
        return converter.to_cdr("sensor_msgs/msg/JointState", false, payload.data(), payload.size(), cdr);
    };

    ConvertResult result = convert(std::vector<std::uint8_t>(9, 0x00));
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 5u);

    std::vector<std::uint8_t> bad_count(9, 0x00);
    bad_count.insert(bad_count.end(), {0x80, 0x80, 0x80});
    result = convert(bad_count);
    EXPECT_EQ(result.fail, ConvertFail::kBadVarint);
    EXPECT_EQ(result.field, 5u);

    std::vector<std::uint8_t> names(9, 0x00);
    names.insert(names.end(), {0x01, 0x01, 'a', 0x01, 0x00, 0x00, 0x00});
    result = convert(names);
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 6u);
}

TEST(ConverterSequences, ConvertsPoseArray)
{
    Converter converter;
    geometry_msgs::msg::PoseArray msg;
    msg.poses.resize(1);

    std::vector<std::uint8_t> payload(9, 0x00);
    payload.push_back(0x01);
    payload.insert(payload.end(), 48, 0x00);
    const std::vector<std::uint8_t> one = le_double(1.0);
    payload.insert(payload.end(), one.begin(), one.end());
    expect_cdr(converter, "geometry_msgs/msg/PoseArray", false, payload, serialize_ref(msg), "pose array");
}

TEST(ConverterSequences, NumbersNestedArrayElementsDynamically)
{
    Converter converter;
    std::vector<std::uint8_t> cdr;
    const auto convert = [&](const std::vector<std::uint8_t> &payload) {
        return converter.to_cdr("geometry_msgs/msg/PoseArray", false, payload.data(), payload.size(), cdr);
    };

    // poses is 5, its first position is 6: cutting inside position.y names 8.
    std::vector<std::uint8_t> mid(9, 0x00);
    mid.push_back(0x01);
    mid.insert(mid.end(), 10, 0x00);
    ConvertResult result = convert(mid);
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 8u);

    // Two poses consume 5 to 14 and 15 to 23: cutting the last byte names 23.
    std::vector<std::uint8_t> two(9, 0x00);
    two.push_back(0x02);
    two.insert(two.end(), 111, 0x00);
    result = convert(two);
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 23u);
}

TEST(ConverterNested, ReportsNestedFieldIndices)
{
    Converter converter;
    std::vector<std::uint8_t> cdr;
    std::vector<std::uint8_t> payload{0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 'i', 'm', 'u'};
    payload.insert(payload.end(), 10, 0x00);
    const ConvertResult result =
        converter.to_cdr("sensor_msgs/msg/Imu", false, payload.data(), payload.size(), cdr);
    EXPECT_EQ(result.fail, ConvertFail::kTruncated);
    EXPECT_EQ(result.field, 7u);
}

TEST(ConverterNested, ConvertsImuWithFixedArrays)
{
    Converter converter;
    sensor_msgs::msg::Imu msg;
    msg.header.stamp.sec = 0x01020304;
    msg.header.frame_id = "imu";

    std::vector<std::uint8_t> payload{0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x03, 'i', 'm', 'u'};
    payload.insert(payload.end(), 3 * 8, 0x00);
    const std::vector<std::uint8_t> one = le_double(1.0);
    payload.insert(payload.end(), one.begin(), one.end());
    payload.insert(payload.end(), 33 * 8, 0x00);
    expect_cdr(converter, "sensor_msgs/msg/Imu", false, payload, serialize_ref(msg), "imu");
}

namespace
{

using rosidl_runtime_cpp::MessageInitialization;
using rosidl_typesupport_introspection_cpp::MessageMember;
using rosidl_typesupport_introspection_cpp::MessageMembers;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_DOUBLE;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_MESSAGE;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_UINT8;
using rosidl_typesupport_introspection_cpp::ROS_TYPE_WSTRING;

void fake_noop_init(void *, MessageInitialization)
{
}

void fake_noop_fini(void *)
{
}

rosidl_message_type_support_t fake_ts_for(const MessageMembers *members)
{
    rosidl_message_type_support_t handle;
    std::memset(&handle, 0, sizeof handle);
    handle.typesupport_identifier = "rosidl_typesupport_introspection_cpp";
    handle.data = members;
    return handle;
}

TEST(ConverterPlans, RefusesKindsWithoutAWireForm)
{
    Converter converter;
    MessageMember member{};
    member.name_ = "data";
    member.type_id_ = ROS_TYPE_WSTRING;
    MessageMembers members{};
    members.message_namespace_ = "fake";
    members.message_name_ = "Wstring";
    members.member_count_ = 1;
    members.size_of_ = sizeof(std::uint16_t);
    members.members_ = &member;
    members.init_function = fake_noop_init;
    members.fini_function = fake_noop_fini;
    const rosidl_message_type_support_t handle = fake_ts_for(&members);
    ASSERT_TRUE(converter.install_plan_for_test("fake/msg/Wstring", &handle));

    std::vector<std::uint8_t> cdr;
    const std::vector<std::uint8_t> payload{0x00};
    const ConvertResult result =
        converter.to_cdr("fake/msg/Wstring", false, payload.data(), payload.size(), cdr);
    EXPECT_EQ(result.fail, ConvertFail::kUnsupportedField);
    EXPECT_EQ(result.field, 0u);
}

TEST(ConverterPlans, RefusesNullHandles)
{
    Converter converter;
    EXPECT_FALSE(converter.install_plan_for_test("fake/msg/Null", nullptr));
    const rosidl_message_type_support_t handle = fake_ts_for(nullptr);
    EXPECT_FALSE(converter.install_plan_for_test("fake/msg/NullData", &handle));

    MessageMember member{};
    member.name_ = "data";
    member.type_id_ = ROS_TYPE_UINT8;
    MessageMembers members{};
    members.message_namespace_ = "fake";
    members.message_name_ = "NoInit";
    members.member_count_ = 1;
    members.size_of_ = 1;
    members.members_ = &member;
    members.fini_function = fake_noop_fini;
    const rosidl_message_type_support_t no_init = fake_ts_for(&members);
    EXPECT_FALSE(converter.install_plan_for_test("fake/msg/NoInit", &no_init));
    members.init_function = fake_noop_init;
    members.fini_function = nullptr;
    const rosidl_message_type_support_t no_fini = fake_ts_for(&members);
    EXPECT_FALSE(converter.install_plan_for_test("fake/msg/NoFini", &no_fini));
}
TEST(ConverterPlans, RefusesMoreThan255Fields)
{
    Converter converter;
    static MessageMember many[256];
    for (auto &member : many) {
        member.name_ = "f";
        member.type_id_ = ROS_TYPE_UINT8;
    }
    MessageMembers members{};
    members.message_namespace_ = "fake";
    members.message_name_ = "Wide";
    members.member_count_ = 256;
    members.size_of_ = 256;
    members.members_ = many;
    members.init_function = fake_noop_init;
    members.fini_function = fake_noop_fini;
    const rosidl_message_type_support_t handle = fake_ts_for(&members);
    EXPECT_FALSE(converter.install_plan_for_test("fake/msg/Wide", &handle));
}

TEST(ConverterPlans, RefusesDeepNesting)
{
    Converter converter;
    static MessageMembers levels[40];
    static MessageMember links[40];
    static rosidl_message_type_support_t nested[40];
    for (int i = 39; i >= 0; --i) {
        nested[i] = fake_ts_for(&levels[i]);
    }
    for (int i = 0; i < 39; ++i) {
        links[i].name_ = "child";
        links[i].type_id_ = ROS_TYPE_MESSAGE;
        links[i].members_ = &nested[i + 1];
        levels[i].message_namespace_ = "fake";
        levels[i].message_name_ = "Deep";
        levels[i].member_count_ = 1;
        levels[i].size_of_ = 1;
        levels[i].members_ = &links[i];
        levels[i].init_function = fake_noop_init;
        levels[i].fini_function = fake_noop_fini;
    }
    links[39].name_ = "leaf";
    links[39].type_id_ = ROS_TYPE_UINT8;
    levels[39].message_namespace_ = "fake";
    levels[39].message_name_ = "Deep";
    levels[39].member_count_ = 1;
    levels[39].size_of_ = 1;
    levels[39].members_ = &links[39];
    levels[39].init_function = fake_noop_init;
    levels[39].fini_function = fake_noop_fini;
    EXPECT_FALSE(converter.install_plan_for_test("fake/msg/Deep", &nested[0]));
}

TEST(ConverterPlans, RefusesMissingArrayAccessors)
{
    Converter converter;
    MessageMember fixed{};
    fixed.name_ = "data";
    fixed.type_id_ = ROS_TYPE_UINT8;
    fixed.is_array_ = true;
    fixed.array_size_ = 3;
    MessageMembers fixed_members{};
    fixed_members.message_namespace_ = "fake";
    fixed_members.message_name_ = "NoGet";
    fixed_members.member_count_ = 1;
    fixed_members.size_of_ = 3;
    fixed_members.members_ = &fixed;
    fixed_members.init_function = fake_noop_init;
    fixed_members.fini_function = fake_noop_fini;
    const rosidl_message_type_support_t fixed_handle = fake_ts_for(&fixed_members);
    EXPECT_FALSE(converter.install_plan_for_test("fake/msg/NoGet", &fixed_handle));

    MessageMember bounded{};
    bounded.name_ = "data";
    bounded.type_id_ = ROS_TYPE_UINT8;
    bounded.is_array_ = true;
    bounded.is_upper_bound_ = true;
    bounded.array_size_ = 5;
    MessageMembers bounded_members{};
    bounded_members.message_namespace_ = "fake";
    bounded_members.message_name_ = "NoResize";
    bounded_members.member_count_ = 1;
    bounded_members.size_of_ = 1;
    bounded_members.members_ = &bounded;
    bounded_members.init_function = fake_noop_init;
    bounded_members.fini_function = fake_noop_fini;
    const rosidl_message_type_support_t bounded_handle = fake_ts_for(&bounded_members);
    EXPECT_FALSE(converter.install_plan_for_test("fake/msg/NoResize", &bounded_handle));
}

TEST(ConverterPlans, RefusesMissingNestedMembers)
{
    Converter converter;
    MessageMember member{};
    member.name_ = "child";
    member.type_id_ = ROS_TYPE_MESSAGE;
    member.members_ = nullptr;
    MessageMembers members{};
    members.message_namespace_ = "fake";
    members.message_name_ = "NoNested";
    members.member_count_ = 1;
    members.size_of_ = 1;
    members.members_ = &member;
    members.init_function = fake_noop_init;
    members.fini_function = fake_noop_fini;
    const rosidl_message_type_support_t handle = fake_ts_for(&members);
    EXPECT_FALSE(converter.install_plan_for_test("fake/msg/NoNested", &handle));
}

struct FakeBounded {
    std::vector<double> values;
};

void fake_bounded_init(void *memory, MessageInitialization)
{
    new (&static_cast<FakeBounded *>(memory)->values) std::vector<double>();
}

void fake_bounded_fini(void *memory)
{
    static_cast<FakeBounded *>(memory)->values.~vector();
}

void *fake_bounded_get(void *memory, std::size_t index)
{
    return &static_cast<FakeBounded *>(memory)->values[index];
}

void fake_bounded_resize(void *memory, std::size_t size)
{
    static_cast<FakeBounded *>(memory)->values.resize(size);
}

TEST(ConverterSequences, RefusesOverBoundSequences)
{
    Converter converter;
    MessageMember member{};
    member.name_ = "data";
    member.type_id_ = ROS_TYPE_DOUBLE;
    member.is_array_ = true;
    member.is_upper_bound_ = true;
    member.array_size_ = 5;
    member.get_function = fake_bounded_get;
    member.resize_function = fake_bounded_resize;
    MessageMembers members{};
    members.message_namespace_ = "fake";
    members.message_name_ = "Bounded";
    members.member_count_ = 1;
    members.size_of_ = sizeof(FakeBounded);
    members.members_ = &member;
    members.init_function = fake_bounded_init;
    members.fini_function = fake_bounded_fini;
    const rosidl_message_type_support_t handle = fake_ts_for(&members);
    ASSERT_TRUE(converter.install_plan_for_test("fake/msg/Bounded", &handle));

    std::vector<std::uint8_t> payload{0x06};
    payload.insert(payload.end(), 6 * 8, 0x00);
    std::vector<std::uint8_t> cdr;
    const ConvertResult result =
        converter.to_cdr("fake/msg/Bounded", false, payload.data(), payload.size(), cdr);
    EXPECT_EQ(result.fail, ConvertFail::kOverBound);
    EXPECT_EQ(result.field, 0u);
}

TEST(ConverterPlans, RefusesConversionWithoutAMiddlewareHandle)
{
    Converter converter;
    MessageMember member{};
    member.name_ = "data";
    member.type_id_ = ROS_TYPE_DOUBLE;
    member.is_array_ = true;
    member.is_upper_bound_ = true;
    member.array_size_ = 5;
    member.get_function = fake_bounded_get;
    member.resize_function = fake_bounded_resize;
    MessageMembers members{};
    members.message_namespace_ = "fake";
    members.message_name_ = "Bounded";
    members.member_count_ = 1;
    members.size_of_ = sizeof(FakeBounded);
    members.members_ = &member;
    members.init_function = fake_bounded_init;
    members.fini_function = fake_bounded_fini;
    const rosidl_message_type_support_t handle = fake_ts_for(&members);
    ASSERT_TRUE(converter.install_plan_for_test("fake/msg/BoundedEmpty", &handle));

    std::vector<std::uint8_t> cdr;
    const std::vector<std::uint8_t> payload{0x00};
    const ConvertResult result =
        converter.to_cdr("fake/msg/BoundedEmpty", false, payload.data(), payload.size(), cdr);
    EXPECT_EQ(result.fail, ConvertFail::kUnsupportedField);
    EXPECT_EQ(result.field, 0xFFu);
}

void *fake_empty_get(void * /*memory*/, std::size_t /*index*/)
{
    return nullptr;
}

TEST(ConverterFieldPath, StopsAtEmptyNestedArrays)
{
    Converter converter;
    MessageMembers empty{};
    empty.message_namespace_ = "fake";
    empty.message_name_ = "Empty";
    empty.member_count_ = 0;
    empty.size_of_ = 1;
    const rosidl_message_type_support_t empty_handle = fake_ts_for(&empty);
    MessageMember member{};
    member.name_ = "items";
    member.type_id_ = ROS_TYPE_MESSAGE;
    member.is_array_ = true;
    member.array_size_ = 2;
    member.members_ = &empty_handle;
    member.get_function = fake_empty_get;
    MessageMembers members{};
    members.message_namespace_ = "fake";
    members.message_name_ = "EmptyArr";
    members.member_count_ = 1;
    members.size_of_ = 1;
    members.members_ = &member;
    members.init_function = fake_noop_init;
    members.fini_function = fake_noop_fini;
    const rosidl_message_type_support_t handle = fake_ts_for(&members);
    ASSERT_TRUE(converter.install_plan_for_test("fake/msg/EmptyArr", &handle));
    EXPECT_EQ(converter.field_path("fake/msg/EmptyArr", 0), "items");
    EXPECT_EQ(converter.field_path("fake/msg/EmptyArr", 1), "");
}

} // namespace
} // namespace test
} // namespace r2s_bridge
