// The router between the MCU's topics and the ROS graph: remapping,
// QoS depth, paced host-to-MCU traffic and converted payloads both ways,
// through a real node with a scripted link and real type support.
// covers: SES-16, SES-18, SES-20, SES-27, SES-31, FRM-11
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <geometry_msgs/msg/twist.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/float32.hpp>

#include "frame_helpers.hpp"
#include "manual_clock.hpp"
#include "r2s_bridge/router.hpp"
#include "rosidl_runtime_c/type_hash.h"

namespace r2s_bridge
{
namespace test
{
namespace
{

constexpr std::uint8_t kBattery = 16u;
constexpr std::uint8_t kCmdVel = 17u;

std::array<std::uint8_t, 8> installed_hash(const std::string &type)
{
    const auto library = rclcpp::get_typesupport_library(type, "rosidl_typesupport_cpp");
    const rosidl_message_type_support_t *handle =
        rclcpp::get_message_typesupport_handle(type, "rosidl_typesupport_cpp", *library);
    const rosidl_type_hash_t *hash = handle->get_type_hash_func(handle);
    std::array<std::uint8_t, 8> out{};
    std::copy(hash->value, hash->value + 8u, out.begin());
    return out;
}

bool logged_has(const std::vector<std::string> &logged, const std::string &part)
{
    return std::any_of(logged.begin(), logged.end(),
                       [&part](const std::string &line) { return line.find(part) != std::string::npos; });
}

class RouterFixture
{
  public:
    RouterFixture(const std::string &name, rclcpp::NodeOptions options, RouterParams params,
                  std::uint16_t max_frame = 512u)
        : max_frame_(max_frame), node_(std::make_shared<rclcpp::Node>(name, "robot1", options))
    {
        router_ = std::make_unique<Router>(
            node_->get_node_topics_interface(), node_->get_logger(),
            [this]() { return std::unique_ptr<Transport>(new HelperLink(transport_)); }, clock_,
            std::move(params), [this](const std::string &text) { logged_.push_back(text); });
    }

    void advance(std::uint32_t ms)
    {
        clock_.advance(std::chrono::milliseconds(ms));
        router_->poll();
    }

    void deliver_frame(std::uint8_t topic_id, std::uint8_t seq, const std::vector<std::uint8_t> &payload)
    {
        std::vector<std::uint8_t> decoded{topic_id, seq};
        decoded.insert(decoded.end(), payload.begin(), payload.end());
        transport_.script_read(helper_wire_of(helper_with_check(decoded)));
        router_->poll();
    }

    // Answers what the session asks until CONNECTED: a Float32 publisher
    // "battery" and a Twist subscription "cmd_vel".
    void handshake(std::uint8_t cmd_vel_flags = 0x01u, std::uint32_t boot_id = 7u)
    {
        deliver_frame(kHelperControl, 0u, helper_hello(boot_id, 2u, max_frame_));
        expect_topic_req(0u);
        const std::array<std::uint8_t, 8> float_hash = installed_hash("std_msgs/msg/Float32");
        deliver_frame(
            kHelperControl, 1u,
            helper_topic(0u, kBattery, 0x00u, float_hash.data(), "std_msgs/msg/Float32", "battery"));
        expect_topic_req(1u);
        const std::array<std::uint8_t, 8> twist_hash = installed_hash("geometry_msgs/msg/Twist");
        deliver_frame(kHelperControl, 2u,
                      helper_topic(1u, kCmdVel, cmd_vel_flags, twist_hash.data(), "geometry_msgs/msg/Twist",
                                   "cmd_vel"));
        drain();
        EXPECT_EQ(router_->publisher_count(), 1u);
        EXPECT_EQ(router_->subscription_count(), 1u);
    }

    // One drained unit, decoded, with its CRC checked.
    std::vector<std::uint8_t> expect_one_frame()
    {
        const std::vector<std::vector<std::uint8_t>> units = drain();
        EXPECT_EQ(units.size(), 1u);
        if (units.size() != 1u) {
            return {};
        }
        const std::vector<std::uint8_t> decoded = checked_decode(units[0]);
        return decoded;
    }

    std::vector<std::uint8_t> checked_decode(const std::vector<std::uint8_t> &unit)
    {
        const std::vector<std::uint8_t> decoded = helper_decode_unit(unit);
        EXPECT_FALSE(decoded.empty()) << "the capture is not rCOBS";
        if (decoded.size() < 4u) {
            return {};
        }
        const std::uint16_t check = static_cast<std::uint16_t>(r2s_crc16_update_buf(
            R2S_CRC16_INIT, decoded.data(), static_cast<std::uint16_t>(decoded.size() - 2u)));
        EXPECT_EQ(static_cast<std::uint8_t>(check & 0xFFu), decoded[decoded.size() - 2u]);
        EXPECT_EQ(static_cast<std::uint8_t>(check >> 8u), decoded[decoded.size() - 1u]);
        return decoded;
    }

    void expect_topic_req(std::uint8_t index)
    {
        // An answered TOPIC leaves its STATUS next to the following REQ.
        const std::vector<std::vector<std::uint8_t>> units = drain();
        ASSERT_FALSE(units.empty());
        const std::vector<std::uint8_t> decoded = checked_decode(units.back());
        ASSERT_GE(decoded.size(), 4u);
        EXPECT_EQ(decoded[0], kHelperControl);
        EXPECT_EQ(decoded[2], 0x03u) << "a TOPIC_REQ";
        EXPECT_EQ(decoded[3], index);
    }

    std::vector<std::vector<std::uint8_t>> drain()
    {
        std::vector<std::uint8_t> all;
        for (const auto &write : transport_.writes()) {
            all.insert(all.end(), write.begin(), write.end());
        }
        transport_.discard_writes();
        return helper_units_of(all);
    }

    Router &router()
    {
        return *router_;
    }
    rclcpp::Node::SharedPtr node()
    {
        return node_;
    }
    const std::vector<std::string> &logged() const
    {
        return logged_;
    }

  private:
    std::uint16_t max_frame_;
    FakeTransport transport_;
    ManualClock clock_;
    rclcpp::Node::SharedPtr node_;
    std::unique_ptr<Router> router_;
    std::vector<std::string> logged_;
};

class RouterNode : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        rclcpp::init(0, nullptr);
    }
    static void TearDownTestSuite()
    {
        rclcpp::shutdown();
    }
};

rclcpp::NodeOptions remapped()
{
    return rclcpp::NodeOptions().arguments({"--ros-args", "-r", "battery:=power/battery"});
}

std::map<std::string, std::string> topic_map(rclcpp::Node::SharedPtr node)
{
    std::map<std::string, std::string> out;
    for (const auto &[name, types] : node->get_topic_names_and_types()) {
        if (!types.empty()) {
            out[name] = types[0];
        }
    }
    return out;
}

TEST_F(RouterNode, RemappingAppliesToTheMcuTopics)
{
    RouterFixture fixture("remap", remapped(), RouterParams{});
    fixture.handshake();
    const std::map<std::string, std::string> topics = topic_map(fixture.node());
    EXPECT_EQ(topics.count("/robot1/power/battery"), 1u) << "the launch remapping renames battery";
    auto cmd_vel = topics.find("/robot1/cmd_vel");
    ASSERT_NE(cmd_vel, topics.end());
    EXPECT_EQ(cmd_vel->second, "geometry_msgs/msg/Twist");
}

TEST_F(RouterNode, QosDepthSetsHistoryDepth)
{
    RouterParams params;
    params.qos_depth = 7;
    RouterFixture fixture("qos", remapped(), params);
    fixture.handshake(0x09u);
    const std::optional<rclcpp::QoS> pub_qos = fixture.router().endpoint_qos(kBattery);
    ASSERT_TRUE(pub_qos.has_value());
    EXPECT_EQ(pub_qos->history(), rclcpp::HistoryPolicy::KeepLast);
    EXPECT_EQ(pub_qos->depth(), 7u);
    EXPECT_EQ(pub_qos->reliability(), rclcpp::ReliabilityPolicy::BestEffort);
    const std::optional<rclcpp::QoS> sub_qos = fixture.router().endpoint_qos(kCmdVel);
    ASSERT_TRUE(sub_qos.has_value());
    EXPECT_EQ(sub_qos->history(), rclcpp::HistoryPolicy::KeepLast);
    EXPECT_EQ(sub_qos->depth(), 7u) << "the reliable flag selects reliable delivery";
    EXPECT_EQ(sub_qos->reliability(), rclcpp::ReliabilityPolicy::Reliable);
    EXPECT_FALSE(fixture.router().endpoint_qos(18u).has_value()) << "no such topic is routed";
}

TEST_F(RouterNode, McuToHostForwardsTheConvertedMessage)
{
    RouterFixture fixture("up", remapped(), RouterParams{});
    fixture.handshake();
    std::vector<float> received;
    // The bridge publishes best effort on this topic ([SES-31]), so the
    // probe subscribes best effort too; a reliable probe would never match.
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "power/battery", rclcpp::QoS(10).best_effort(),
        [&received](const std_msgs::msg::Float32 &msg) { received.push_back(msg.data); });
    fixture.deliver_frame(kBattery, 0u, {0x00, 0x00, 0xC0, 0x3F});
    rclcpp::spin_some(fixture.node());
    ASSERT_EQ(received.size(), 1u);
    EXPECT_FLOAT_EQ(received[0], 1.5F);
}

TEST_F(RouterNode, HostToMcuSendsThePackedMessage)
{
    RouterFixture fixture("down", remapped(), RouterParams{});
    fixture.handshake();
    auto pub = fixture.node()->create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
    geometry_msgs::msg::Twist twist;
    twist.linear.x = 1.0;
    pub->publish(twist);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    const std::vector<std::uint8_t> decoded = fixture.expect_one_frame();
    ASSERT_GE(decoded.size(), 4u);
    EXPECT_EQ(decoded[0], kCmdVel);
    EXPECT_EQ(decoded[1], 0u) << "the first frame on a topic carries seq 0";
    std::vector<std::uint8_t> want{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF0, 0x3F};
    want.resize(48u, 0x00u);
    EXPECT_EQ(std::vector<std::uint8_t>(decoded.begin() + 2u, decoded.end() - 2u), want);
}

TEST_F(RouterNode, BadPayloadLogsTheTopicAndField)
{
    RouterFixture fixture("bad", remapped(), RouterParams{});
    fixture.handshake();
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "power/battery", rclcpp::QoS(10).best_effort(),
        [&received](const std_msgs::msg::Float32 &msg) { received.push_back(msg.data); });
    fixture.deliver_frame(kBattery, 0u, {0x00});
    rclcpp::spin_some(fixture.node());
    EXPECT_TRUE(received.empty()) << "a truncated payload is dropped, never published";
    EXPECT_TRUE(logged_has(fixture.logged(), "battery")) << "the drop names the topic";
    EXPECT_TRUE(logged_has(fixture.logged(), "data")) << "the drop names the field path";
}

TEST_F(RouterNode, OversizeRosMessageIsDropped)
{
    RouterFixture fixture("big", remapped(), RouterParams{}, 20u);
    fixture.handshake();
    auto pub = fixture.node()->create_publisher<geometry_msgs::msg::Twist>("cmd_vel", 10);
    geometry_msgs::msg::Twist twist;
    pub->publish(twist);
    rclcpp::spin_some(fixture.node());
    fixture.advance(1u);
    EXPECT_TRUE(fixture.drain().empty()) << "a frame past max_frame never goes out";
    EXPECT_TRUE(logged_has(fixture.logged(), "cmd_vel")) << "the drop names the topic";
}

TEST_F(RouterNode, IdenticalRehandshakeKeepsRoutes)
{
    RouterFixture fixture("re", remapped(), RouterParams{});
    fixture.handshake();
    std::vector<float> received;
    auto sub = fixture.node()->create_subscription<std_msgs::msg::Float32>(
        "power/battery", rclcpp::QoS(10).best_effort(),
        [&received](const std_msgs::msg::Float32 &msg) { received.push_back(msg.data); });
    fixture.handshake(0x01u, 8u);
    EXPECT_EQ(fixture.router().publisher_count(), 1u);
    EXPECT_EQ(fixture.router().subscription_count(), 1u);
    fixture.deliver_frame(kBattery, 0u, {0x00, 0x00, 0xC0, 0x3F});
    rclcpp::spin_some(fixture.node());
    ASSERT_EQ(received.size(), 1u) << "traffic flows again after the re-handshake";
    EXPECT_FLOAT_EQ(received[0], 1.5F);
}

TEST_F(RouterNode, RejectsNonsenseParams)
{
    RouterParams no_depth;
    no_depth.qos_depth = 0;
    EXPECT_THROW(RouterFixture("p1", remapped(), no_depth), std::invalid_argument);
    RouterParams no_budget;
    no_budget.tx_budget_pct = 101;
    EXPECT_THROW(RouterFixture("p2", remapped(), no_budget), std::invalid_argument);
    RouterParams no_baud;
    no_baud.baud = 0;
    EXPECT_THROW(RouterFixture("p3", remapped(), no_baud), std::invalid_argument);
}

TEST(RouterOverrides, ParsesTopicRates)
{
    std::vector<std::string> logged;
    const std::map<std::string, double> rates = parse_rate_overrides(
        {"cmd_vel=20", "battery=0.5"}, [&logged](const std::string &text) { logged.push_back(text); });
    EXPECT_EQ(rates.size(), 2u);
    EXPECT_DOUBLE_EQ(rates.at("cmd_vel"), 20.0);
    EXPECT_DOUBLE_EQ(rates.at("battery"), 0.5);
    EXPECT_TRUE(logged.empty());
}

TEST(RouterOverrides, SkipsMalformedEntries)
{
    std::vector<std::string> logged;
    const std::map<std::string, double> rates = parse_rate_overrides(
        {"bogus", "cmd_vel=-1", "again=1=2"}, [&logged](const std::string &text) { logged.push_back(text); });
    EXPECT_TRUE(rates.empty());
    EXPECT_TRUE(logged_has(logged, "bogus"));
    EXPECT_TRUE(logged_has(logged, "cmd_vel=-1"));
}

} // namespace
} // namespace test
} // namespace r2s_bridge
