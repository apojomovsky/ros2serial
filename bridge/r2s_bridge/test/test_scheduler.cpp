// The host-to-MCU pacer: keep-latest, per-topic rates, the global bandwidth
// budget, round-robin service and bounded queues (spec/protocol.md, 8.4).
// covers: SES-26, SES-27, SES-28, SES-29, SES-30
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "r2s_bridge/scheduler.hpp"

namespace r2s_bridge
{
namespace test
{
namespace
{

using ns = std::chrono::nanoseconds;
using sec = std::chrono::duration<double>;

struct Capture {
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> sent;
    void operator()(std::uint8_t id, const std::vector<std::uint8_t> &payload)
    {
        sent.emplace_back(id, payload);
    }
};

SchedulerTopic queued_topic(std::uint8_t id)
{
    SchedulerTopic topic;
    topic.topic_id = id;
    topic.keep_latest = false;
    topic.rate_hz = 0.0;
    topic.queue_bound = 10;
    return topic;
}

TEST(SchedulerKeepLatest, KeepsOnlyTheNewest)
{
    Scheduler scheduler;
    SchedulerTopic topic;
    topic.topic_id = 16;
    topic.keep_latest = true;
    topic.queue_bound = 1;
    scheduler.configure({topic}, 1e9);
    scheduler.submit(16, {0x01});
    scheduler.submit(16, {0x02});
    scheduler.submit(16, {0x03});
    EXPECT_EQ(scheduler.queued(16), 1u);

    Capture capture;
    scheduler.poll(ns(0), std::ref(capture));
    ASSERT_EQ(capture.sent.size(), 1u);
    EXPECT_EQ(capture.sent[0].first, 16u);
    EXPECT_EQ(capture.sent[0].second, (std::vector<std::uint8_t>{0x03}));
}

TEST(SchedulerRate, SpacesFrames)
{
    Scheduler scheduler;
    SchedulerTopic topic = queued_topic(16);
    topic.rate_hz = 1.0;
    scheduler.configure({topic}, 1e9);
    scheduler.submit(16, {0x0A});
    scheduler.submit(16, {0x0B});

    Capture first;
    scheduler.poll(ns(0), std::ref(first));
    ASSERT_EQ(first.sent.size(), 1u) << "the first frame goes out at once";
    EXPECT_EQ(first.sent[0].second, (std::vector<std::uint8_t>{0x0A}));

    Capture blocked;
    scheduler.poll(ns(0), std::ref(blocked));
    EXPECT_TRUE(blocked.sent.empty()) << "the second frame waits out its second";

    Capture paced;
    scheduler.poll(ns(1'000'000'000), std::ref(paced));
    ASSERT_EQ(paced.sent.size(), 1u);
    EXPECT_EQ(paced.sent[0].second, (std::vector<std::uint8_t>{0x0B}));
}

TEST(SchedulerBudget, CapsBytesPerSecond)
{
    Scheduler scheduler;
    scheduler.configure({queued_topic(16), queued_topic(17)}, 10.0);
    // Four packed bytes ride in ten wire bytes, the whole budget for a second.
    scheduler.submit(16, {0x01, 0x02, 0x03, 0x04});
    scheduler.submit(17, {0x05, 0x06, 0x07, 0x08});

    Capture first;
    scheduler.poll(ns(0), std::ref(first));
    ASSERT_EQ(first.sent.size(), 1u) << "the budget fits one frame";
    EXPECT_EQ(first.sent[0].first, 16u);

    Capture starved;
    scheduler.poll(ns(0), std::ref(starved));
    EXPECT_TRUE(starved.sent.empty()) << "no byte left for the second topic";

    Capture refilled;
    scheduler.poll(ns(1'000'000'000), std::ref(refilled));
    ASSERT_EQ(refilled.sent.size(), 1u);
    EXPECT_EQ(refilled.sent[0].first, 17u);
}

TEST(SchedulerBound, MatchesTheWireRule)
{
    // A packed payload rides with its topic id, seq and CRC: four more
    // bytes before the rCOBS code bytes and the delimiter.
    EXPECT_EQ(Scheduler::encoded_frame_bound(0), 6u);
    EXPECT_EQ(Scheduler::encoded_frame_bound(4), 10u);
    EXPECT_EQ(Scheduler::encoded_frame_bound(254), 261u);
    EXPECT_EQ(Scheduler::encoded_frame_bound(255), 262u);
}

TEST(SchedulerRoundRobin, ServesTopicsInTurn)
{
    Scheduler scheduler;
    scheduler.configure({queued_topic(16), queued_topic(17), queued_topic(18)}, 1e9);
    scheduler.submit(17, {0x11});
    scheduler.submit(18, {0x12});
    scheduler.submit(16, {0x10});

    Capture pass;
    scheduler.poll(ns(0), std::ref(pass));
    ASSERT_EQ(pass.sent.size(), 3u) << "one frame per topic in a pass";
    EXPECT_EQ(pass.sent[0].first, 16u);
    EXPECT_EQ(pass.sent[1].first, 17u);
    EXPECT_EQ(pass.sent[2].first, 18u);

    scheduler.submit(16, {0x20});
    Capture wrapped;
    scheduler.poll(ns(0), std::ref(wrapped));
    ASSERT_EQ(wrapped.sent.size(), 1u) << "the cursor wrapped and still serves";
    EXPECT_EQ(wrapped.sent[0].first, 16u);
}

TEST(SchedulerQueue, DropsOldestPastBound)
{
    Scheduler scheduler;
    SchedulerTopic topic = queued_topic(16);
    topic.queue_bound = 2;
    scheduler.configure({topic}, 1e9);
    scheduler.submit(16, {0x0A});
    scheduler.submit(16, {0x0B});
    scheduler.submit(16, {0x0C});
    EXPECT_EQ(scheduler.dropped(16), 1u);

    Capture pass;
    scheduler.poll(ns(0), std::ref(pass));
    ASSERT_EQ(pass.sent.size(), 2u);
    EXPECT_EQ(pass.sent[0].second, (std::vector<std::uint8_t>{0x0B}));
    EXPECT_EQ(pass.sent[1].second, (std::vector<std::uint8_t>{0x0C}));
}

TEST(SchedulerBound, MatchesTheWireRule)
{
    EXPECT_EQ(Scheduler::encoded_frame_bound(0), 2u);
    EXPECT_EQ(Scheduler::encoded_frame_bound(4), 6u);
    EXPECT_EQ(Scheduler::encoded_frame_bound(254), 257u);
    EXPECT_EQ(Scheduler::encoded_frame_bound(255), 258u);
}

TEST(SchedulerConfig, RejectsANegativeBudget)
{
    Scheduler scheduler;
    EXPECT_THROW(scheduler.configure({queued_topic(16)}, -1.0), std::invalid_argument);
}

TEST(SchedulerSubmit, IgnoresAnUnknownTopic)
{
    Scheduler scheduler;
    scheduler.configure({queued_topic(16)}, 1e9);
    scheduler.submit(17, {0x01});
    Capture capture;
    scheduler.poll(ns(0), std::ref(capture));
    EXPECT_TRUE(capture.sent.empty());
}

} // namespace
} // namespace test
} // namespace r2s_bridge
