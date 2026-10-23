#include <memory>

#include <rclcpp/rclcpp.hpp>

#include "r2s_bridge/bridge.hpp"

int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<r2s_bridge::Bridge>());
    rclcpp::shutdown();
    return 0;
}
