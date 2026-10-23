#include <gtest/gtest.h>

#include <class_loader/class_loader.hpp>
#include <rclcpp/rclcpp.hpp>
#include <rclcpp_components/node_factory.hpp>

#include <memory>
#include <string>

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
