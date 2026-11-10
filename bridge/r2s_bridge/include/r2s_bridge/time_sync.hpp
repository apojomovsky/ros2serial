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

// The host side of time sync: offset and drift from PONG samples
// (spec/protocol.md, sections 7.2 and 8.3). Pure counts in, correction out;
// the session owns the wire frames and the schedule, the bridge owns the ROS
// clock the correction stamps.
#ifndef R2S_BRIDGE__TIME_SYNC_HPP_
#define R2S_BRIDGE__TIME_SYNC_HPP_

#include <cstddef>
#include <cstdint>

namespace r2s_bridge
{

/// A CORRECTION payload parsed.
/**
 * The anchor tick, the ROS time at that tick, and the drift in
 * nanoseconds of ROS time per nominal MCU millisecond.
 */
struct TimeCorrection
{
  std::uint32_t tick0 = 0;  ///< The anchor tick.
  std::int32_t sec = 0;  ///< The ROS seconds at the anchor.
  std::uint32_t nanosec = 0;  ///< The ROS nanoseconds at the anchor.
  std::int16_t drift_ppm = 0;  ///< The drift at the anchor.
};

/// The host side of the time sync estimator.
/**
 * Pure counts in, correction out; the session owns the wire frames and
 * the schedule, the bridge owns the ROS clock the correction stamps.
 */
class TimeSync
{
public:
  /// The estimator window (SES-24).
  /**
   * The 16 most recent samples, the offset from the 4 with the lowest
   * round trip. Ties keep the most recent, so the estimate tracks a
   * clock that jumped instead of sticking to history.
   */
  static constexpr std::size_t kWindow = 16;

  /// How many lowest round trip samples the offset fits.
  static constexpr std::size_t kFitCount = 4;

  /// A correction moved by more than this counts as moved (SES-25).
  static constexpr std::int64_t kMoveThresholdNs = 100000;

  /// The drift the wire carries, in either direction (TIM-4).
  static constexpr std::int16_t kMaxDriftPpm = 20000;

  /// Starts the estimator for a tick rate.
  /**
   * \param[in] tick_hz the MCU ticks per second.
   * \throws std::invalid_argument unless tick_hz is 1000 times a divisor
   * of 10^6, so a tick converts to nanoseconds with integer math alone
   * (CTL-8).
   */
  explicit TimeSync(std::uint32_t tick_hz);

  /// Forgets every sample and the anchor.
  void reset();

  /// One answered PING: the host steady send and receive times.
  /**
   * The host ROS time at the receive, and the MCU tick the PONG carries.
   * The midpoint of each pair halves the round trip onto the arrival
   * (TIM-1), which assumes the delay splits evenly; the lowest round
   * trip samples carry the least of that skew. The session feeds every
   * field from its two clocks.
   *
   * \param[in] send_ns the host steady time at the PING send.
   * \param[in] recv_ns the host steady time at the PONG receive.
   * \param[in] ros_recv_ns the host ROS time at the PONG receive.
   * \param[in] tick the MCU tick the PONG carries.
   */
  void add_sample(
    std::int64_t send_ns, std::int64_t recv_ns, std::int64_t ros_recv_ns,
    std::uint32_t tick);

  /// Whether any sample anchored a correction.
  /**
   * \return True once a sample arrived.
   */
  bool has_estimate() const;

  /// The correction for the latest tick.
  /**
   * False before the first sample.
   *
   * \param[out] out the correction.
   * \return True when a correction is anchored.
   */
  bool correction(TimeCorrection & out) const;

  /// Whether a fresh correction moved past the last one (SES-25).
  /**
   * Moved means more than kMoveThresholdNs past the ROS time the last
   * one predicts for the latest tick. False before the first sample.
   *
   * \param[in] last the correction to compare.
   * \return True when the fresh correction moved.
   */
  bool moved_since(const TimeCorrection & last) const;

  /// The H8 diagnostics, valid once has_estimate holds.
  /**
   * The fitted ROS time at MCU tick zero, the fitted drift, the latest
   * round trip. The offset stays put while the drift carries the anchor,
   * so it names the clock phase where the drift names its rate.
   *
   * \return The fitted offset in nanoseconds.
   */
  std::int64_t offset_ns() const;

  /// The fitted drift of the H8 diagnostics.
  /**
   * \return The drift.
   */
  std::int16_t drift_ppm() const;

  /// The latest round trip of the H8 diagnostics.
  /**
   * \return The round trip in nanoseconds.
   */
  std::int64_t round_trip_ns() const;

private:
  struct Estimate
  {
    bool valid = false;
    std::int16_t drift = 0;
    std::int64_t base = 0;
    std::int64_t ros_ns = 0;
  };

  Estimate estimate() const;

  std::uint32_t ns_per_tick_ = 0u;
  struct Sample
  {
    std::int64_t ros_mid_ns = 0;
    std::int64_t rtt_ns = 0;
    std::uint64_t tick = 0;
  };
    // The window, oldest first; the ticks unwrap the 32-bit counter, so the
    // wraparound never looks like a drift step.
  Sample samples_[kWindow]{};
  std::size_t count_ = 0;
  std::size_t head_ = 0;
    // The raw tick of the previous sample, for the unwrap, and the unwrapped
    // absolute tick of the latest one the offset anchors to.
  std::uint32_t prev_raw_ = 0;
  std::uint64_t latest_tick_ = 0;
  bool has_base_ = false;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__TIME_SYNC_HPP_
