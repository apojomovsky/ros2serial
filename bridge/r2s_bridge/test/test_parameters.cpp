// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include <rclcpp/rclcpp.hpp>

#include "r2s_bridge/bridge.hpp"

class BridgeParameters : public ::testing::Test
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

TEST_F(BridgeParameters, DeclaresEverySpecifiedParameterWithItsDefault)
{
    auto node = std::make_shared<r2s_bridge::Bridge>();
    const std::vector<rclcpp::Parameter> expected{
    rclcpp::Parameter("port", "/dev/ttyACM0"),
    rclcpp::Parameter("baud", 115200),
    rclcpp::Parameter("low_latency", true),
    rclcpp::Parameter("hupcl", true),
    rclcpp::Parameter("boot_wait_ms", 2500),
    rclcpp::Parameter("node_map", ""),
    rclcpp::Parameter("reconnect_period_ms", 1000),
    rclcpp::Parameter("heartbeat_period_ms", 500),
    rclcpp::Parameter("link_timeout_ms", 2000),
    rclcpp::Parameter("time_sync.enabled", true),
    rclcpp::Parameter("time_sync.period_ms", 5000),
    rclcpp::Parameter("rate_limit.default_hz", 0.0),
    rclcpp::Parameter("rate_limit.overrides", std::vector<std::string>{}),
    rclcpp::Parameter("tx_budget_pct", 50),
    rclcpp::Parameter("qos_depth", 10),
    };

    for (const auto & want : expected) {
    SCOPED_TRACE(want.get_name());
    ASSERT_TRUE(node->has_parameter(want.get_name()));
    EXPECT_EQ(node->get_parameter(want.get_name()), want);
    }
}
