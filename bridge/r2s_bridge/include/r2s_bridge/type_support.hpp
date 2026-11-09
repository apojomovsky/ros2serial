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

// The topic verdict: loads the installed type support of one announced
// topic and checks the MCU's hash against it ([SES-16], [CTL-12], [CTL-13]).
#ifndef R2S_BRIDGE__TYPE_SUPPORT_HPP_
#define R2S_BRIDGE__TYPE_SUPPORT_HPP_

#include <cstdint>

#include "r2s_bridge/session.hpp"

namespace r2s_bridge
{

/// The TOPIC_STATUS verdict for one announced topic.
/**
 * ok when the installed hash matches, unknown type when the type does not
 * load, hash mismatch otherwise. Never throws; a failed load is verdict
 * 1. Needs no node: it opens the installed library and closes it again
 * when done.
 *
 * \param[in] info the announced topic to judge.
 * \return The verdict, one of the kTopicStatus codes.
 */
std::uint8_t resolve_topic_type(const TopicInfo & info);

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__TYPE_SUPPORT_HPP_
