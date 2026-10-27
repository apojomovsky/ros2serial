// The installed type support answers one announced topic: accepted, unknown,
// or hash mismatch (spec/protocol.md, section 8.2).
// covers: SES-16, CTL-12, CTL-13
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include "r2s_bridge/session.hpp"
#include "r2s_bridge/type_support.hpp"

namespace r2s_bridge
{
namespace
{

// The first 8 wire bytes of the RIHS01 hash the installed type support
// reports for std_msgs/msg/Float32. The type has no dependencies, so its
// hash cannot drift between the supported distros.
constexpr std::array<std::uint8_t, 8> kFloat32Hash{0x71u, 0x70u, 0xd3u, 0xd8u, 0xf8u, 0x41u, 0xf7u, 0xbeu};

TopicInfo announced(std::string type, std::array<std::uint8_t, 8> hash)
{
    TopicInfo info;
    info.topic_id = 0x10u;
    info.type_hash = hash;
    info.type_name = std::move(type);
    info.topic_name = "battery";
    return info;
}

} // namespace

TEST(TypeSupport, AcceptsTheInstalledHash)
{
    EXPECT_EQ(resolve_topic_type(announced("std_msgs/msg/Float32", kFloat32Hash)), kTopicStatusOk);
}

TEST(TypeSupport, RejectsAModifiedHash)
{
    auto hash = kFloat32Hash;
    hash.back() ^= 0x01u;
    EXPECT_EQ(resolve_topic_type(announced("std_msgs/msg/Float32", hash)), kTopicStatusHashMismatch);
}

TEST(TypeSupport, RejectsAnUnknownPackage)
{
    EXPECT_EQ(resolve_topic_type(announced("no_such_pkg/msg/Nope", kFloat32Hash)), kTopicStatusUnknownType);
}

TEST(TypeSupport, RejectsAMalformedTypeName)
{
    EXPECT_EQ(resolve_topic_type(announced("Float32", kFloat32Hash)), kTopicStatusUnknownType);
}

TEST(TopicStatus, CodesMatchTheSpec)
{
    EXPECT_EQ(kTopicStatusOk, 0u);
    EXPECT_EQ(kTopicStatusUnknownType, 1u);
    EXPECT_EQ(kTopicStatusHashMismatch, 2u);
    EXPECT_EQ(kTopicStatusNameConflict, 3u);
}

} // namespace r2s_bridge
