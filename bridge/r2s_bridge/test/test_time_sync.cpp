// The host time-sync estimator: offset from the lowest round trip samples,
// drift from a linear fit, corrections the MCU can anchor r2s_now() to.
// covers: SES-24, SES-25
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>

#include "r2s_bridge/time_sync.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

constexpr std::uint32_t kTickHz = 1000000u;
constexpr std::int64_t kTickNs = 1000;
// A correction moves by more than this to count as moved (SES-25).
constexpr std::int64_t kMoveNs = 100000;
// The scenario bound e2e checks end to end: corrected MCU time stays inside.
constexpr std::int64_t kScenarioBoundNs = 1000000;

// A drifting MCU on a symmetric link: the tick advances (1 + drift) per
// steady nanosecond, ROS trails steady by a fixed skew, every PONG takes the
// same round trip on the wire.
struct SimMcu {
    double drift = 0.0;
    std::int64_t skew_ns = 0;
    // The ROS clock's own rate error: its nanoseconds per steady nanosecond
    // minus one, for the sim-time case where ROS and steady disagree.
    double ros_drift = 0.0;
    std::int64_t rtt_ns = 2000000;

    std::uint32_t tick_at(std::int64_t steady_ns) const
    {
        return static_cast<std::uint32_t>(std::llround(steady_ns / kTickNs * (1.0 + drift)));
    }

    std::int64_t ros_at(std::int64_t steady_ns) const
    {
        return static_cast<std::int64_t>(std::llround(steady_ns * (1.0 + ros_drift))) + skew_ns;
    }

    // One PONG answered at steady_ns, fed straight into the estimator. The
    // tick is stamped at the arrival, halfway through the round trip.
    void sample(TimeSync &sync, std::int64_t steady_ns) const
    {
        sync.add_sample(steady_ns, steady_ns + rtt_ns, ros_at(steady_ns + rtt_ns),
                        tick_at(steady_ns + rtt_ns / 2));
    }

    std::int64_t true_ros(std::uint32_t tick) const
    {
        return static_cast<std::int64_t>(std::llround(tick * kTickNs / (1.0 + drift) * (1.0 + ros_drift))) +
               skew_ns;
    }
};

// The section 7.2 arithmetic, mirroring r2s_gen.reference.time.compute: the
// ROS time the MCU reads under a correction, or false past the anchor.
// The correction as one nanosecond count, for the estimate checks.
std::int64_t ros_ns_of(const TimeCorrection &correction)
{
    return static_cast<std::int64_t>(correction.sec) * 1000000000 + correction.nanosec;
}

bool mcu_time(const TimeCorrection &c, std::uint32_t tick, std::int64_t &sec, std::int64_t &nsec)
{
    const std::uint32_t k = kTickHz / 1000u;
    const std::uint32_t e = tick - c.tick0;
    const std::uint32_t ms = e / k;
    if (ms > 10000u) {
        return false;
    }
    const std::uint32_t p = 1000000u / k;
    sec = c.sec + static_cast<std::int64_t>(ms / 1000u);
    std::int64_t ns = static_cast<std::int64_t>(c.nanosec) +
                      static_cast<std::int64_t>((ms % 1000u) * 1000000u + (e % k) * p);
    sec += ns / 1000000000;
    ns %= 1000000000;
    ns += static_cast<std::int64_t>(ms) * c.drift_ppm;
    while (ns < 0) {
        ns += 1000000000;
        --sec;
    }
    while (ns >= 1000000000) {
        ns -= 1000000000;
        ++sec;
    }
    nsec = ns;
    return true;
}

TEST(TimeSyncConfig, RejectsATickHzOutsideCtl8)
{
    EXPECT_THROW(TimeSync(0u), std::invalid_argument);
    EXPECT_THROW(TimeSync(999u), std::invalid_argument);
    EXPECT_THROW(TimeSync(1500u), std::invalid_argument);
    EXPECT_THROW(TimeSync(3000u), std::invalid_argument) << "k=3 does not divide 10^6";
    EXPECT_NO_THROW(TimeSync(1000u));
    EXPECT_NO_THROW((void)TimeSync(kTickHz));
}

TEST(TimeSyncEstimate, HasNoEstimateBeforeTheFirstSample)
{
    TimeSync sync(kTickHz);
    EXPECT_FALSE(sync.has_estimate());
    TimeCorrection out;
    EXPECT_FALSE(sync.correction(out));
    EXPECT_FALSE(sync.moved_since(out));
}

TEST(TimeSyncEstimate, CorrectsExactlyOnASymmetricLinkWithoutDrift)
{
    TimeSync sync(kTickHz);
    SimMcu mcu;
    for (int i = 0; i < 8; ++i) {
        mcu.sample(sync, static_cast<std::int64_t>(i) * 50000000);
    }
    // Symmetric delay puts the arrival midpoint exactly on the PONG tick,
    // which the MCU stamps halfway through the round trip.
    TimeCorrection out;
    ASSERT_TRUE(sync.correction(out));
    const std::uint32_t tick0 = mcu.tick_at(7 * 50000000 + mcu.rtt_ns / 2);
    EXPECT_EQ(out.tick0, tick0);
    EXPECT_EQ(ros_ns_of(out), mcu.true_ros(tick0));
    EXPECT_EQ(sync.offset_ns(), 0);
    EXPECT_EQ(sync.drift_ppm(), 0);
    EXPECT_EQ(sync.round_trip_ns(), mcu.rtt_ns);
}

TEST(TimeSyncEstimate, FollowsAFixedRosSkew)
{
    TimeSync sync(kTickHz);
    SimMcu mcu;
    mcu.skew_ns = 2000000000;
    for (int i = 0; i < 8; ++i) {
        mcu.sample(sync, static_cast<std::int64_t>(i) * 50000000);
    }
    TimeCorrection out;
    ASSERT_TRUE(sync.correction(out));
    EXPECT_EQ(ros_ns_of(out), mcu.true_ros(out.tick0));
    EXPECT_EQ(sync.offset_ns(), mcu.skew_ns);
}

TEST(TimeSyncEstimate, Tracks5000ppmOfDriftInsideTheScenarioBound)
{
    TimeSync sync(kTickHz);
    SimMcu mcu;
    mcu.drift = 5000.0 / 1000000.0;
    mcu.skew_ns = 500000000;
    // Sixteen PINGs five seconds apart: the full SES-24 window of drift.
    for (int i = 0; i < 16; ++i) {
        mcu.sample(sync, static_cast<std::int64_t>(i) * 5000000000);
    }
    TimeCorrection out;
    ASSERT_TRUE(sync.correction(out));
    // The fit measures ROS nanoseconds per nominal MCU millisecond, so a
    // clock 1.005 times too fast reads (1 / 1.005 - 1) of that, not -5000.
    // Tick quantization is half a microsecond per sample, far below one ppm
    // over the 75 s the fit spans.
    const int want = static_cast<int>(std::llround((1.0 / (1.0 + mcu.drift) - 1.0) * 1000000.0));
    EXPECT_NEAR(sync.drift_ppm(), want, 2);
    // The phase offset stays at the skew instead of absorbing the drift: the
    // drift term carries the anchor on its own.
    EXPECT_NEAR(sync.offset_ns(), mcu.skew_ns, 100000);
    // The MCU reads within the scenario bound five seconds past the anchor.
    const std::uint32_t later = out.tick0 + 5000000u;
    std::int64_t sec = 0;
    std::int64_t nsec = 0;
    ASSERT_TRUE(mcu_time(out, later, sec, nsec));
    EXPECT_NEAR(sec * 1000000000 + nsec, mcu.true_ros(later), kScenarioBoundNs);
}

TEST(TimeSyncEstimate, TracksTheRosRateNotTheSteadyRate)
{
    TimeSync sync(kTickHz);
    SimMcu mcu;
    // ROS runs 1% fast while the MCU is exact: the drift the correction
    // carries is the ROS rate, +10000 ppm ([TIM-4]).
    mcu.ros_drift = 0.01;
    for (int i = 0; i < 8; ++i) {
        mcu.sample(sync, static_cast<std::int64_t>(i) * 50000000);
    }
    TimeCorrection out;
    ASSERT_TRUE(sync.correction(out));
    EXPECT_NEAR(sync.drift_ppm(), 10000, 2);
    EXPECT_EQ(out.tick0, mcu.tick_at(7 * 50000000 + mcu.rtt_ns / 2));
    // The midpoint halves the steady round trip, so a ROS rate off the
    // steady rate biases the anchor by half the round trip's own rate
    // difference: 2 ms at 1% is 10 us. The slope, from differences, stays
    // exact.
    const std::int64_t err = ros_ns_of(out) - mcu.true_ros(out.tick0);
    EXPECT_NEAR(err > 0 ? err : -err, mcu.rtt_ns * mcu.ros_drift / 2, 1000);
}

TEST(TimeSyncEstimate, ForgetsSamplesPastTheWindow)
{
    TimeSync sync(kTickHz);
    SimMcu mcu;
    for (int i = 0; i < 16; ++i) {
        mcu.sample(sync, static_cast<std::int64_t>(i) * 5000000000);
    }
    TimeCorrection still;
    ASSERT_TRUE(sync.correction(still));
    EXPECT_EQ(still.drift_ppm, 0);
    SimMcu fast = mcu;
    fast.drift = 8000.0 / 1000000.0;
    for (int i = 16; i < 32; ++i) {
        fast.sample(sync, static_cast<std::int64_t>(i) * 5000000000);
    }
    TimeCorrection out;
    ASSERT_TRUE(sync.correction(out));
    const int want = static_cast<int>(std::llround((1.0 / (1.0 + fast.drift) - 1.0) * 1000000.0));
    EXPECT_NEAR(sync.drift_ppm(), want, 2) << "only the 16 most recent samples fit";
}

TEST(TimeSyncEstimate, TheLowestRoundTripSamplesSetTheOffset)
{
    TimeSync sync(kTickHz);
    SimMcu mcu;
    // Even samples fly symmetric in 1 ms; odd samples queue 5 ms extra on the
    // way back, biasing their midpoint by half of that.
    for (int i = 0; i < 16; ++i) {
        const std::int64_t at = static_cast<std::int64_t>(i) * 5000000000;
        if (i % 2 == 0) {
            mcu.sample(sync, at);
        } else {
            sync.add_sample(at, at + mcu.rtt_ns + 5000000, mcu.ros_at(at + mcu.rtt_ns + 5000000),
                            mcu.tick_at(at));
        }
    }
    TimeCorrection out;
    ASSERT_TRUE(sync.correction(out));
    // Averaging all sixteen would sit 1.25 ms high; the quartile stays near.
    EXPECT_NEAR(ros_ns_of(out), mcu.true_ros(out.tick0), 500000);
}

TEST(TimeSyncEstimate, SurvivesATickWraparound)
{
    TimeSync sync(kTickHz);
    SimMcu mcu;
    // One millisecond per sample at 1 MHz, starting a thousand ticks below
    // the wrap.
    const std::uint32_t start = 0xFFFFFFFFu - 999u;
    for (int i = 0; i < 8; ++i) {
        const std::uint32_t tick = start + static_cast<std::uint32_t>(i) * 1000u;
        const std::int64_t at = static_cast<std::int64_t>(i) * 1000000;
        sync.add_sample(at, at + mcu.rtt_ns, mcu.ros_at(at + mcu.rtt_ns), tick);
    }
    TimeCorrection out;
    ASSERT_TRUE(sync.correction(out));
    EXPECT_EQ(out.drift_ppm, 0);
    EXPECT_EQ(ros_ns_of(out), 7 * 1000000 + mcu.rtt_ns / 2);
}

TEST(TimeSyncEstimate, ClampsTheDriftTo20000ppm)
{
    // A fast clock packs more ticks into each real millisecond, so the ROS
    // nanoseconds to add per nominal millisecond come out negative.
    TimeSync fast(kTickHz);
    SimMcu mcu;
    mcu.drift = 50000.0 / 1000000.0;
    for (int i = 0; i < 16; ++i) {
        mcu.sample(fast, static_cast<std::int64_t>(i) * 5000000000);
    }
    TimeCorrection out;
    ASSERT_TRUE(fast.correction(out));
    EXPECT_EQ(out.drift_ppm, -20000);
    TimeSync slow(kTickHz);
    mcu.drift = -50000.0 / 1000000.0;
    for (int i = 0; i < 16; ++i) {
        mcu.sample(slow, static_cast<std::int64_t>(i) * 5000000000);
    }
    ASSERT_TRUE(slow.correction(out));
    EXPECT_EQ(out.drift_ppm, 20000);
}

TEST(TimeSyncEstimate, ReportsMovedPast100us)
{
    TimeSync sync(kTickHz);
    SimMcu mcu;
    for (int i = 0; i < 8; ++i) {
        mcu.sample(sync, static_cast<std::int64_t>(i) * 50000000);
    }
    TimeCorrection sent;
    ASSERT_TRUE(sync.correction(sent));
    EXPECT_FALSE(sync.moved_since(sent));
    // The MCU clock jumps 300 us: four fresh samples carry the new offset
    // into the lowest round trip quartile, past the bar.
    SimMcu jumped = mcu;
    jumped.skew_ns += 3 * kMoveNs;
    for (int i = 8; i < 12; ++i) {
        jumped.sample(sync, static_cast<std::int64_t>(i) * 50000000);
    }
    EXPECT_TRUE(sync.moved_since(sent));
    // An 80 us jump stays under the bar.
    TimeSync small(kTickHz);
    for (int i = 0; i < 8; ++i) {
        mcu.sample(small, static_cast<std::int64_t>(i) * 50000000);
    }
    TimeCorrection base;
    ASSERT_TRUE(small.correction(base));
    SimMcu drifted = mcu;
    drifted.skew_ns += 80000;
    for (int i = 8; i < 12; ++i) {
        drifted.sample(small, static_cast<std::int64_t>(i) * 50000000);
    }
    EXPECT_FALSE(small.moved_since(base));
}

TEST(TimeSyncEstimate, ResetClearsTheEstimate)
{
    TimeSync sync(kTickHz);
    SimMcu mcu;
    mcu.sample(sync, 0);
    ASSERT_TRUE(sync.has_estimate());
    sync.reset();
    EXPECT_FALSE(sync.has_estimate());
    TimeCorrection out;
    EXPECT_FALSE(sync.correction(out));
}

} // namespace
} // namespace test
} // namespace r2s_bridge
