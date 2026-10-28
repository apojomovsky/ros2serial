// The host time-sync estimator: offset from the lowest round trip samples,
// drift from a linear fit, corrections the MCU anchors r2s_now() to.
#include "r2s_bridge/time_sync.hpp"

#include <cmath>
#include <stdexcept>

namespace r2s_bridge
{
TimeSync::TimeSync(std::uint32_t tick_hz) : ns_per_tick_(0u)
{
    if (tick_hz < 1000u || tick_hz % 1000u != 0u) {
        throw std::invalid_argument("r2s_bridge: tick_hz is not 1000 times a divisor of 10^6");
    }
    const std::uint32_t per_ms = tick_hz / 1000u;
    if (1000000u % per_ms != 0u) {
        throw std::invalid_argument("r2s_bridge: tick_hz is not 1000 times a divisor of 10^6");
    }
    ns_per_tick_ = 1000000u / per_ms;
}

void TimeSync::reset()
{
    count_ = 0;
    head_ = 0;
    has_base_ = false;
}

void TimeSync::add_sample(std::int64_t send_ns, std::int64_t recv_ns, std::int64_t ros_recv_ns,
                          std::uint32_t tick)
{
    const std::int64_t rtt = recv_ns - send_ns;
    Sample sample;
    sample.mid_ns = recv_ns - rtt / 2;
    sample.ros_mid_ns = ros_recv_ns - rtt / 2;
    sample.rtt_ns = rtt;
    if (!has_base_) {
        latest_tick_ = tick;
        prev_raw_ = tick;
        has_base_ = true;
    } else {
        // Unsigned wraparound counts forward through the 32-bit wrap, so a
        // PONG pair straddling it stays one step apart.
        latest_tick_ += static_cast<std::uint32_t>(tick - prev_raw_);
        prev_raw_ = tick;
    }
    sample.tick = latest_tick_;
    if (count_ < kWindow) {
        samples_[(head_ + count_) % kWindow] = sample;
        ++count_;
    } else {
        samples_[head_] = sample;
        head_ = (head_ + 1u) % kWindow;
    }
}

bool TimeSync::has_estimate() const
{
    return count_ > 0u;
}

TimeSync::Estimate TimeSync::estimate() const
{
    Estimate out;
    if (count_ == 0u) {
        return out;
    }
    // Tick nanoseconds from the window base: small enough for int64, exact in
    // a double, so the fit never sees the absolute tick origin.
    double x[kWindow]{};
    double y[kWindow]{};
    double rtt[kWindow]{};
    const std::uint64_t base = samples_[head_].tick;
    for (std::size_t i = 0; i < count_; ++i) {
        const Sample &sample = samples_[(head_ + i) % kWindow];
        const auto elapsed = static_cast<double>(sample.tick - base) * ns_per_tick_;
        x[i] = elapsed;
        y[i] = static_cast<double>(sample.mid_ns) - elapsed;
        rtt[i] = static_cast<double>(sample.rtt_ns);
    }
    double sum_x = 0.0;
    for (std::size_t i = 0; i < count_; ++i) {
        sum_x += x[i];
    }
    const double mean_x = sum_x / static_cast<double>(count_);
    double sum_y = 0.0;
    for (std::size_t i = 0; i < count_; ++i) {
        sum_y += y[i];
    }
    const double mean_y = sum_y / static_cast<double>(count_);
    double sxx = 0.0;
    double sxy = 0.0;
    for (std::size_t i = 0; i < count_; ++i) {
        sxx += (x[i] - mean_x) * (x[i] - mean_x);
        sxy += (x[i] - mean_x) * (y[i] - mean_y);
    }
    const double slope = sxx > 0.0 ? sxy / sxx : 0.0;
    // The lowest round trip samples name the offset; a newer sample wins a
    // tie, so a jumped clock reaches the quartile instead of waiting behind
    // older samples with the same round trip.
    bool take[kWindow]{};
    std::size_t taken = 0;
    const std::size_t want = count_ < kFitCount ? count_ : kFitCount;
    // Newest-first pass: the first want entries with the lowest round trips.
    for (std::size_t pass = 0; pass < count_; ++pass) {
        const std::size_t i = count_ - 1u - pass;
        if (taken < want) {
            take[i] = true;
            ++taken;
            continue;
        }
        std::size_t worst = count_;
        for (std::size_t j = 0; j < count_; ++j) {
            if (take[j] && (worst == count_ || rtt[j] > rtt[worst])) {
                worst = j;
            }
        }
        if (worst != count_ && rtt[i] < rtt[worst]) {
            take[worst] = false;
            take[i] = true;
        }
    }
    double ros_offset = 0.0;
    for (std::size_t i = 0; i < count_; ++i) {
        if (!take[i]) {
            continue;
        }
        const Sample &sample = samples_[(head_ + i) % kWindow];
        ros_offset += static_cast<double>(sample.ros_mid_ns) - x[i] - slope * x[i];
    }
    ros_offset /= static_cast<double>(taken);
    const Sample &latest = samples_[(head_ + count_ - 1u) % kWindow];
    const double x_latest = static_cast<double>(latest.tick - base) * ns_per_tick_;
    const std::int64_t ros_ns =
        static_cast<std::int64_t>(std::llround(ros_offset + x_latest + slope * x_latest));
    out.valid = true;
    out.slope = slope;
    const double ppm = slope * 1000000.0;
    out.drift = static_cast<std::int16_t>(
        ppm > kMaxDriftPpm ? kMaxDriftPpm : (ppm < -kMaxDriftPpm ? -kMaxDriftPpm : std::llround(ppm)));
    out.ros_ns = ros_ns;
    return out;
}

bool TimeSync::correction(TimeCorrection &out) const
{
    const Estimate fit = estimate();
    if (!fit.valid) {
        return false;
    }
    const std::uint32_t tick0 = static_cast<std::uint32_t>(latest_tick_ & 0xFFFFFFFFu);
    std::int64_t sec = fit.ros_ns / 1000000000;
    std::int64_t nsec = fit.ros_ns % 1000000000;
    if (nsec < 0) {
        nsec += 1000000000;
        --sec;
    }
    out.tick0 = tick0;
    out.sec = static_cast<std::int32_t>(sec);
    out.nanosec = static_cast<std::uint32_t>(nsec);
    out.drift_ppm = fit.drift;
    return true;
}

bool TimeSync::moved_since(const TimeCorrection &last) const
{
    const Estimate fit = estimate();
    if (!fit.valid) {
        return false;
    }
    // Forward ticks from the last anchor; a correction older than half the
    // counter range is ancient history the floor resends anyway.
    const auto delta_ticks = static_cast<double>(
        static_cast<std::uint32_t>(static_cast<std::uint32_t>(latest_tick_ & 0xFFFFFFFFu) - last.tick0));
    const double predict = static_cast<double>(last.sec) * 1000000000.0 + static_cast<double>(last.nanosec) +
                           delta_ticks * ns_per_tick_ * (1.0 + last.drift_ppm / 1000000.0);
    return std::fabs(static_cast<double>(fit.ros_ns) - predict) > static_cast<double>(kMoveThresholdNs);
}

std::int64_t TimeSync::offset_ns() const
{
    const Estimate fit = estimate();
    if (!fit.valid) {
        return 0;
    }
    return fit.ros_ns - static_cast<std::int64_t>(latest_tick_ * ns_per_tick_);
}

std::int16_t TimeSync::drift_ppm() const
{
    const Estimate fit = estimate();
    return fit.valid ? fit.drift : 0;
}

std::int64_t TimeSync::round_trip_ns() const
{
    if (count_ == 0u) {
        return 0;
    }
    return samples_[(head_ + count_ - 1u) % kWindow].rtt_ns;
}

} // namespace r2s_bridge
