// The host-to-MCU pacer: keep-latest, per-topic rates, the global bandwidth
// budget, round-robin service and bounded queues (spec/protocol.md, 8.4).
// covers: SES-26, SES-27, SES-28, SES-29, SES-30, SES-32
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
struct Capture {
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> sent;
    bool refuse = false;
    std::vector<std::uint8_t> attempted;
    bool operator()(std::uint8_t id, const std::vector<std::uint8_t> &payload)
    {
        attempted.push_back(id);
        if (refuse) {
            return false;
        }
        sent.emplace_back(id, payload);
        return true;
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
    EXPECT_EQ(scheduler.dropped(16), 2u) << "both replacements are dropped and counted";
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

TEST(SchedulerBudget, BurstCarriesOneLargeFrame)
{
    Scheduler scheduler;
    // A 10 byte per second budget with room for one 30 byte burst: three ten
    // byte frames go out at once, the fourth waits for the refill.
    scheduler.configure({queued_topic(16), queued_topic(17), queued_topic(18)}, 10.0, 30.0);
    scheduler.submit(16, {0x01, 0x02, 0x03, 0x04});
    scheduler.submit(17, {0x05, 0x06, 0x07, 0x08});
    scheduler.submit(18, {0x09, 0x0A, 0x0B, 0x0C});

    Capture burst;
    scheduler.poll(ns(0), std::ref(burst));
    ASSERT_EQ(burst.sent.size(), 3u) << "the burst carries the whole round";

    scheduler.submit(16, {0x0D, 0x0E, 0x0F, 0x10});
    Capture starved;
    scheduler.poll(ns(0), std::ref(starved));
    EXPECT_TRUE(starved.sent.empty()) << "the burst is spent, the refill is not due";

    Capture refilled;
    scheduler.poll(ns(2'000'000'000), std::ref(refilled));
    ASSERT_EQ(refilled.sent.size(), 1u);
    EXPECT_EQ(refilled.sent[0].first, 16u);
}

TEST(SchedulerBudget, RefreshesTheBurst)
{
    Scheduler scheduler;
    scheduler.configure({queued_topic(16)}, 10.0, 30.0);
    EXPECT_THROW(scheduler.set_max_burst(-1.0), std::invalid_argument);
    scheduler.submit(16, {0x01, 0x02, 0x03, 0x04});
    Capture sent;
    scheduler.poll(ns(0), std::ref(sent));
    ASSERT_EQ(sent.sent.size(), 1u) << "the burst carries the frame";
    scheduler.set_max_burst(5.0);
    scheduler.submit(16, {0x05, 0x06, 0x07, 0x08});
    Capture shrunk;
    scheduler.poll(ns(0), std::ref(shrunk));
    ASSERT_EQ(shrunk.sent.size(), 1u) << "ten bytes still fit ten bytes";
    scheduler.submit(16, {0x09, 0x0A, 0x0B, 0x0C});
    Capture blocked;
    scheduler.poll(ns(0), std::ref(blocked));
    EXPECT_TRUE(blocked.sent.empty()) << "the shrunk bucket holds no second frame";
}

TEST(SchedulerBackpressure, StopsThePassOnRefusal)
{
    Scheduler scheduler;
    scheduler.configure({queued_topic(16), queued_topic(17)}, 1e9);
    scheduler.submit(16, {0x01});
    scheduler.submit(17, {0x02});

    Capture refusing;
    refusing.refuse = true;
    scheduler.poll(ns(0), std::ref(refusing));
    EXPECT_EQ(refusing.attempted, (std::vector<std::uint8_t>{16u})) << "the pass stops at the first refusal";
    EXPECT_EQ(scheduler.queued(16), 1u) << "the refused frame stays queued";

    Capture sent;
    scheduler.poll(ns(0), std::ref(sent));
    ASSERT_EQ(sent.sent.size(), 2u);
    EXPECT_EQ(sent.sent[0].first, 16u);
    EXPECT_EQ(sent.sent[1].first, 17u);
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

TEST(SchedulerConfig, RejectsANegativeRate)
{
    Scheduler scheduler;
    SchedulerTopic topic = queued_topic(16);
    topic.rate_hz = -1.0;
    EXPECT_THROW(scheduler.configure({topic}, 1e9), std::invalid_argument);
}

TEST(SchedulerSubmit, CountsNothingForAnUnknownTopic)
{
    Scheduler scheduler;
    scheduler.configure({queued_topic(16)}, 1e9);
    scheduler.submit(17, {0x01});
    EXPECT_EQ(scheduler.queued(99u), 0u);
    EXPECT_EQ(scheduler.dropped(99u), 0u);
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
    Capture first;
    scheduler.poll(ns(0), std::ref(first));
    ASSERT_EQ(first.sent.size(), 1u) << "one frame per topic per pass";
    EXPECT_EQ(first.sent[0].second, (std::vector<std::uint8_t>{0x0B}));

    Capture second;
    scheduler.poll(ns(0), std::ref(second));
    ASSERT_EQ(second.sent.size(), 1u);
    EXPECT_EQ(second.sent[0].second, (std::vector<std::uint8_t>{0x0C}));
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
