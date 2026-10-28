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

// A CORRECTION payload parsed: the anchor tick, the ROS time at that tick,
// and the drift in nanoseconds of ROS time per nominal MCU millisecond.
struct TimeCorrection {
    std::uint32_t tick0 = 0;
    std::int32_t sec = 0;
    std::uint32_t nanosec = 0;
    std::int16_t drift_ppm = 0;
};

class TimeSync
{
  public:
    // The estimator window (SES-24): the 16 most recent samples, the offset
    // from the 4 with the lowest round trip. Ties keep the most recent, so
    // the estimate tracks a clock that jumped instead of sticking to history.
    static constexpr std::size_t kWindow = 16;
    static constexpr std::size_t kFitCount = 4;
    // A correction moved by more than this counts as moved (SES-25).
    static constexpr std::int64_t kMoveThresholdNs = 100000;
    // The drift the wire carries, in either direction (TIM-4).
    static constexpr std::int16_t kMaxDriftPpm = 20000;

    // Throws std::invalid_argument unless tick_hz is 1000 times a divisor of
    // 10^6, so a tick converts to nanoseconds with integer math alone (CTL-8).
    explicit TimeSync(std::uint32_t tick_hz);

    void reset();

    // One answered PING: the host steady send and receive times, the host ROS
    // time at the receive, and the MCU tick the PONG carries. The midpoint of
    // each pair halves the round trip onto the arrival (TIM-1), which assumes
    // the delay splits evenly; the lowest round trip samples carry the least
    // of that skew. The session feeds every field from its two clocks.
    void add_sample(std::int64_t send_ns, std::int64_t recv_ns, std::int64_t ros_recv_ns, std::uint32_t tick);

    bool has_estimate() const;
    // The correction for the latest tick: false before the first sample.
    bool correction(TimeCorrection &out) const;
    // Whether a fresh correction moved more than kMoveThresholdNs past the
    // ROS time the last one predicts for the latest tick (SES-25). False
    // before the first sample.
    bool moved_since(const TimeCorrection &last) const;

    // The H8 diagnostics, valid once has_estimate holds: the ROS-minus-MCU
    // offset at the latest anchor, the fitted drift, the latest round trip.
    std::int64_t offset_ns() const;
    std::int16_t drift_ppm() const;
    std::int64_t round_trip_ns() const;

  private:
    struct Estimate {
        bool valid = false;
        double slope = 0.0;
        std::int16_t drift = 0;
        std::int64_t ros_ns = 0;
    };

    Estimate estimate() const;

    std::uint32_t ns_per_tick_ = 0u;
    struct Sample {
        std::int64_t mid_ns = 0;
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

} // namespace r2s_bridge

#endif // R2S_BRIDGE__TIME_SYNC_HPP_
