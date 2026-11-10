// The service introspection handles: names, sizes, live structs and the
// unknown-type refusal behind every routed service.
// covers: SVR-3
#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include <rclcpp/rclcpp.hpp>

#include "r2s_bridge/service_types.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

TEST(ServiceTypes, LoadsTriggerHandles)
{
  const ServiceTypes types("std_srvs/srv/Trigger");
  EXPECT_EQ(types.request_type_name(), "std_srvs/srv/Trigger_Request");
  EXPECT_EQ(types.response_type_name(), "std_srvs/srv/Trigger_Response");
  EXPECT_GT(types.request_size(), 0u);
  EXPECT_GT(types.response_size(), 0u);
  EXPECT_NE(types.service_handle(), nullptr);
  EXPECT_NE(types.cpp_service_handle(), nullptr);
}

TEST(ServiceTypes, RejectsUnknownType)
{
  EXPECT_THROW(ServiceTypes("nope/msg/Nope"), std::runtime_error);
}

TEST(ServiceTypes, MovesAndFreesStructs)
{
  const ServiceTypes types("std_srvs/srv/Trigger");
  ServiceTypes::Struct first = types.make_request();
  ServiceTypes::Struct second = types.make_request();
  const std::size_t size = types.request_size();
  EXPECT_EQ(first.storage.size(), size);
  // Assigning over a live struct finalizes it first; the source goes empty.
  second = std::move(first);
  EXPECT_EQ(second.storage.size(), size);
  EXPECT_EQ(first.storage.size(), 0u);
  // Assigning over an empty struct skips the finalizer.
  ServiceTypes::Struct third;
  third = std::move(second);
  EXPECT_EQ(third.storage.size(), size);
  EXPECT_EQ(second.storage.size(), 0u);
  // A members-set struct with emptied storage finalizes nothing on the way
  // out, on assignment or at scope end.
  ServiceTypes::Struct live = types.make_request();
  live.storage.clear();
  ServiceTypes::Struct target;
  target = std::move(live);
  EXPECT_EQ(target.storage.size(), 0u);
  EXPECT_NE(target.members, nullptr);
}

}  // namespace
}  // namespace test
}  // namespace r2s_bridge

int main(int argc, char ** argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  rclcpp::init(argc, argv);
  const int code = RUN_ALL_TESTS();
  rclcpp::shutdown();
  return code;
}
