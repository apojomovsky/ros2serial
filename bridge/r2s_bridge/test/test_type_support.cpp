// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The installed type support answers one announced topic: accepted, unknown,
// or hash mismatch (spec/protocol.md, section 8.2).
// covers: SES-16, CTL-12, CTL-13
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

#include "r2s_bridge/action_types.hpp"
#include "r2s_bridge/session.hpp"
#include "r2s_bridge/type_support.hpp"

namespace r2s_bridge
{
namespace
{

// The first 8 wire bytes of the RIHS01 hash the installed type support
// reports for std_msgs/msg/Float32. The type has no dependencies, so its
// hash cannot drift between the supported distros.
constexpr std::array<std::uint8_t, 8> kFloat32Hash{0x71u, 0x70u, 0xd3u, 0xd8u, 0xf8u, 0x41u, 0xf7u,
  0xbeu};

TopicInfo announced(std::string type, std::array<std::uint8_t, 8> hash)
{
  TopicInfo info;
  info.topic_id = 0x10u;
  info.type_hash = hash;
  info.type_name = std::move(type);
  info.topic_name = "battery";
  return info;
}

}  // namespace

TEST(TypeSupport, AcceptsTheInstalledHash)
{
    EXPECT_EQ(resolve_topic_type(announced("std_msgs/msg/Float32", kFloat32Hash)), kTopicStatusOk);
}

TEST(TypeSupport, RejectsAModifiedHash)
{
    for (std::size_t i = 0; i < kFloat32Hash.size(); ++i) {
    auto hash = kFloat32Hash;
    hash[i] ^= 0x01u;
    EXPECT_EQ(resolve_topic_type(announced("std_msgs/msg/Float32", hash)), kTopicStatusHashMismatch)
            << "flipping wire byte " << i << " must reject the topic";
    }
}

TEST(TypeSupport, RejectsAnUnknownPackage)
{
    EXPECT_EQ(resolve_topic_type(announced("no_such_pkg/msg/Nope", kFloat32Hash)),
      kTopicStatusUnknownType);
}

TEST(TypeSupport, RejectsAMalformedTypeName)
{
    EXPECT_EQ(resolve_topic_type(announced("Float32", kFloat32Hash)), kTopicStatusUnknownType);
}

TEST(TopicInfo, InequalityNamesEachField)
{
    TopicInfo base;
    base.topic_id = 0x10u;
    base.flags = 0x01u;
    base.type_hash = kFloat32Hash;
    base.type_name = "std_msgs/msg/Float32";
    base.topic_name = "battery";
    EXPECT_TRUE(base == base);
    TopicInfo changed = base;
    changed.topic_id = 0x11u;
    EXPECT_FALSE(base == changed);
    changed = base;
    changed.flags = 0x02u;
    EXPECT_FALSE(base == changed);
    changed = base;
    changed.type_hash[0] ^= 0x01u;
    EXPECT_FALSE(base == changed);
    changed = base;
    changed.type_name = "std_msgs/msg/Int32";
    EXPECT_FALSE(base == changed);
    changed = base;
    changed.topic_name = "cell";
    EXPECT_FALSE(base == changed);
}

TEST(TopicStatus, CodesMatchTheSpec)
{
    EXPECT_EQ(kTopicStatusOk, 0u);
    EXPECT_EQ(kTopicStatusUnknownType, 1u);
    EXPECT_EQ(kTopicStatusHashMismatch, 2u);
    EXPECT_EQ(kTopicStatusNameConflict, 3u);
}

// The installed type support answers one announced action the same way
// (spec/protocol.md, 7.9).
// covers: ACT-7
ActionInfo announced_action(std::string type, std::array<std::uint8_t, 8> hash)
{
  ActionInfo info;
  info.index = 0u;
  info.type_hash = hash;
  info.type_name = std::move(type);
  info.action_name = "spin";
  return info;
}

TEST(ActionResolve, AcceptsTheInstalledHash)
{
    const ActionTypes types("tf2_msgs/action/LookupTransform");
    EXPECT_EQ(resolve_action_type(announced_action("tf2_msgs/action/LookupTransform",
        types.type_hash())), kActionStatusOk);
}

TEST(ActionResolve, RejectsAModifiedHash)
{
    const ActionTypes types("tf2_msgs/action/LookupTransform");
    for (std::size_t i = 0; i < 8u; ++i) {
    auto hash = types.type_hash();
    hash[i] ^= 0x01u;
    EXPECT_EQ(resolve_action_type(announced_action("tf2_msgs/action/LookupTransform", hash)),
        kActionStatusHashMismatch) << "flipping wire byte " << i << " must refuse the action";
    }
}

TEST(ActionResolve, RejectsAnUnknownPackage)
{
    const ActionTypes types("tf2_msgs/action/LookupTransform");
    EXPECT_EQ(resolve_action_type(announced_action("no_such_pkg/action/Nope", types.type_hash())),
      kActionStatusUnknownType);
}

TEST(ActionStatus, CodesMatchTheSpec)
{
    EXPECT_EQ(kActionStatusOk, 0u);
    EXPECT_EQ(kActionStatusUnknownType, 1u);
    EXPECT_EQ(kActionStatusHashMismatch, 2u);
    EXPECT_EQ(kActionStatusNameConflict, 3u);
}

TEST(ActionInfo, InequalityNamesEachField)
{
    ActionInfo base;
    base.index = 0x03u;
    base.type_hash = {1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u};
    base.type_name = "tf2_msgs/action/LookupTransform";
    base.action_name = "spin";
    EXPECT_TRUE(base == base);
    EXPECT_FALSE(base != base);
    ActionInfo changed = base;
    changed.index = 0x04u;
    EXPECT_TRUE(base != changed);
    changed = base;
    changed.type_hash[0] ^= 0x01u;
    EXPECT_TRUE(base != changed);
    changed = base;
    changed.type_name = "tf2_msgs/action/FollowJointTrajectory";
    EXPECT_TRUE(base != changed);
    changed = base;
    changed.action_name = "nudge";
    EXPECT_TRUE(base != changed);
    changed = base;
    changed.status = kActionStatusUnknownType;
    EXPECT_TRUE(base != changed);
}

}  // namespace r2s_bridge
