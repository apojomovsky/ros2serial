// The bridge publishes /diagnostics from its poll timer: a Bridge on a pty
// reports boot_wait before any HELLO and handshaking after one, with the
// session values as key/value pairs.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <pty.h>
#include <unistd.h>

#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <rclcpp/rclcpp.hpp>

#include "frame_helpers.hpp"
#include "r2s_bridge/bridge.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

// A pty pair the bridge opens as its port: the test writes MCU bytes to the
// master side. Throws when the pair cannot be made.
class Pty
{
  public:
    Pty()
    {
        if (openpty(&master_, &slave_, nullptr, nullptr, nullptr) != 0) {
            throw std::runtime_error(std::string("openpty failed: ") + std::strerror(errno));
        }
        char name[64];
        if (ptsname_r(master_, name, sizeof name) != 0) {
            throw std::runtime_error(std::string("ptsname_r failed: ") + std::strerror(errno));
        }
        path_ = name;
        ::close(slave_);
        slave_ = -1;
    }
    ~Pty()
    {
        if (master_ >= 0) {
            ::close(master_);
        }
    }
    Pty(const Pty &) = delete;
    Pty &operator=(const Pty &) = delete;

    const std::string &path() const
    {
        return path_;
    }

    void write_all(const std::vector<std::uint8_t> &bytes)
    {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const ssize_t done = ::write(master_, bytes.data() + offset, bytes.size() - offset);
            if (done <= 0) {
                throw std::runtime_error(std::string("pty write failed: ") + std::strerror(errno));
            }
            offset += static_cast<std::size_t>(done);
        }
    }

  private:
    int master_ = -1;
    int slave_ = -1;
    std::string path_;
};

std::map<std::string, std::string> values_of(const diagnostic_msgs::msg::DiagnosticArray &message)
{
    std::map<std::string, std::string> out;
    for (const diagnostic_msgs::msg::KeyValue &value : message.status[0].values) {
        out[value.key] = value.value;
    }
    return out;
}

class BridgeDiagnostics : public ::testing::Test
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

TEST_F(BridgeDiagnostics, PublishesBootWaitThenHandshaking)
{
    Pty pty;
    rclcpp::NodeOptions options;
    options.parameter_overrides(
        {rclcpp::Parameter("port", pty.path()), rclcpp::Parameter("boot_wait_ms", 60000)});
    auto bridge = std::make_shared<r2s_bridge::Bridge>(options);
    auto probe = std::make_shared<rclcpp::Node>("diag_probe");
    std::vector<diagnostic_msgs::msg::DiagnosticArray> received;
    auto sub = probe->create_subscription<diagnostic_msgs::msg::DiagnosticArray>(
        "/diagnostics", 10,
        [&received](diagnostic_msgs::msg::DiagnosticArray msg) { received.push_back(std::move(msg)); });
    const auto both = [&bridge, &probe]() {
        rclcpp::spin_some(bridge);
        rclcpp::spin_some(probe);
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received.empty() && std::chrono::steady_clock::now() < deadline) {
        both();
    }
    ASSERT_FALSE(received.empty()) << "the bridge publishes /diagnostics on its poll timer";
    // boot_wait_ms outlasts any loaded poll loop, so the first message still
    // reports the wait even when polling slows down.
    EXPECT_EQ(received.back().status[0].message, "boot_wait");
    EXPECT_EQ(values_of(received.back()).at("session token"), "0");
    EXPECT_NE(received.back().header.stamp, builtin_interfaces::msg::Time())
        << "the bridge stamps what the builder leaves zero";

    std::vector<std::uint8_t> decoded{kHelperControl, 0u};
    const std::vector<std::uint8_t> hello = helper_hello(7u, 2u);
    decoded.insert(decoded.end(), hello.begin(), hello.end());
    pty.write_all(helper_wire_of(helper_with_check(decoded)));
    const auto shaking = [&received]() {
        return !received.empty() && received.back().status[0].message == "handshaking";
    };
    const auto moved = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!shaking() && std::chrono::steady_clock::now() < moved) {
        both();
    }
    ASSERT_TRUE(shaking()) << "a HELLO moves the published link state";
    const std::map<std::string, std::string> values = values_of(received.back());
    EXPECT_EQ(values.at("session token"), "7") << "the MCU echo is reported until READY assigns one";
    EXPECT_EQ(values.at("resets"), "0");
    EXPECT_EQ(received.back().status[0].hardware_id, pty.path());
}

} // namespace
} // namespace test
} // namespace r2s_bridge
