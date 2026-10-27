// The topic verdict: loads the installed type support of one announced
// topic and checks the MCU's hash against it ([SES-16], [CTL-12], [CTL-13]).
#ifndef R2S_BRIDGE__TYPE_SUPPORT_HPP_
#define R2S_BRIDGE__TYPE_SUPPORT_HPP_

#include <cstdint>

#include "r2s_bridge/session.hpp"

namespace r2s_bridge
{

// The TOPIC_STATUS verdict for one announced topic: ok when the installed
// hash matches, unknown type when the type does not load, hash mismatch
// otherwise. Never throws; a failed load is verdict 1. Needs no node: it
// opens the installed library and closes it again when done.
std::uint8_t resolve_topic_type(const TopicInfo &info);

} // namespace r2s_bridge

#endif // R2S_BRIDGE__TYPE_SUPPORT_HPP_
