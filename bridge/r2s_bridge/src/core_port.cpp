// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The core's port seam for the bridge: one slot per live link, each with
// the transport and clock one FrameReader's framer instance reads through.
#include "core_port.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

extern "C" {
#include "r2s/r2s_port.h"
}

namespace
{
// The tick is the clock's whole milliseconds, so it wraps every 49 days and
// stays an integer division like the MCU's tick_hz math.
constexpr std::int64_t kNanosecondsPerTick = 1000000;

struct Slot
{
  r2s_bridge::Transport *transport = nullptr;
  const r2s_bridge::Clock *clock = nullptr;
  bool fault = false;
};

static_assert(R2S_MAX_LINKS > 1, "the bridge frames several links in one process");
std::array<Slot, R2S_MAX_LINKS> slots;
}  // namespace

namespace r2s_bridge
{

std::uint8_t attach_core_port(Transport & transport, const Clock & clock)
{
  for (std::size_t link = 0; link < slots.size(); link++) {
    if (slots[link].transport == nullptr) {
      slots[link].transport = &transport;
      slots[link].clock = &clock;
      slots[link].fault = false;
      return static_cast<std::uint8_t>(link);
    }
  }
  throw std::logic_error("no free core port slot: at most " + std::to_string(R2S_MAX_LINKS) +
                           " live links per process");
}

void detach_core_port(std::uint8_t link)
{
  slots[link].transport = nullptr;
  slots[link].clock = nullptr;
  slots[link].fault = false;
}

bool link_faulted(std::uint8_t link)
{
  return slots[link].fault;
}

void clear_link_fault(std::uint8_t link)
{
  slots[link].fault = false;
}

}  // namespace r2s_bridge

extern "C" uint16_t r2s_port_read_link(uint8_t link, uint8_t *buf, uint16_t max)
{
  Slot & slot = slots[link];
  if (slot.transport == nullptr) {
    return 0u;
  }
  const r2s_bridge::IoResult got = slot.transport->read(buf, static_cast<std::size_t>(max));
  if (got.status == r2s_bridge::IoStatus::kOk) {
    return static_cast<uint16_t>(got.bytes);
  }
  if (got.status != r2s_bridge::IoStatus::kWouldBlock) {
    slot.fault = true;
  }
  return 0u;
}

extern "C" uint32_t r2s_port_tick_link(uint8_t link)
{
  const Slot & slot = slots[link];
  if (slot.clock == nullptr) {
    return 0u;
  }
  return static_cast<uint32_t>(slot.clock->now().count() / kNanosecondsPerTick);
}

extern "C" uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    // The singleton's link: single-link callers keep working without change.
  return r2s_port_read_link(0u, buf, max);
}

extern "C" uint32_t r2s_port_tick(void)
{
  return r2s_port_tick_link(0u);
}
