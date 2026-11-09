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

#include <class_loader/class_loader.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_components/node_factory.hpp>

#ifndef R2S_BRIDGE_COMPONENT_LIBRARY
#error "R2S_BRIDGE_COMPONENT_LIBRARY must name the built component library"
#endif

class ComponentLoad : public ::testing::Test
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

TEST_F(ComponentLoad, LoadsTheBridgeFromTheBuiltLibrary)
{
    class_loader::ClassLoader loader(R2S_BRIDGE_COMPONENT_LIBRARY);
    const auto names = loader.getAvailableClasses<rclcpp_components::NodeFactory>();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names.front(), "rclcpp_components::NodeFactoryTemplate<r2s_bridge::Bridge>");

    auto factory = loader.createInstance<rclcpp_components::NodeFactory>(names.front());
    ASSERT_NE(factory, nullptr);
    auto node = factory->create_node_instance(rclcpp::NodeOptions()).get_node_base_interface();
    ASSERT_NE(node, nullptr);
    EXPECT_EQ(std::string(node->get_name()), "r2s_bridge");
}
