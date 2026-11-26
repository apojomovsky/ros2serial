// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// Fuzz target: the bridge session on arbitrary decoded frames, driven by
// bridge_session_harness.hpp through the fake transport and the manual
// clock. The session must take any frame without crashing, hanging or
// touching memory out of bounds; the sanitizers are the oracle. The
// transcript seeds open with a valid HELLO, so a seeded run gets past the
// handshake into the TOPIC, PARAM, SVC and ACT walks and connected traffic.
#include <cstddef>
#include <cstdint>

#include "bridge_session_harness.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t * data, std::size_t size)
{
  r2s_bridge::fuzz::drive_bridge_session(data, size);
  return 0;
}
