// Host-to-MCU pacing: per-topic queues with keep-latest, rate limits and a
// global bandwidth budget, served round-robin. A plain C++ class over a
// nanosecond clock, so tests drive it without rclcpp (spec/protocol.md, 8.4).
#ifndef R2S_BRIDGE__SCHEDULER_HPP_
#define R2S_BRIDGE__SCHEDULER_HPP_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace r2s_bridge
{

// One paced topic. rate_hz of 0 sets no limit ([SES-27]); the queue holds
// queue_bound messages, or one when keep_latest is set ([SES-26], [SES-30]).
struct SchedulerTopic {
    std::uint8_t topic_id = 0;
    bool keep_latest = false;
    double rate_hz = 0.0;
    std::size_t queue_bound = 1;
};

// The pacing state. Single-threaded; the caller serializes submit and poll.
class Scheduler
{
  public:
    // Replaces every topic and clears queues, buckets and counters.
    // max_burst_bytes raises the bucket capacity to at least one frame of
    // the caller's maximum (see [SES-28]). Throws std::invalid_argument on
    // a negative rate, budget or burst.
    void configure(std::vector<SchedulerTopic> topics, double budget_bytes_per_sec,
                   double max_burst_bytes = 0.0);

    // Refreshes the burst capacity after a re-handshake that kept the routes:
    // the MCU's new maximum frame may exceed the old bucket ([SES-28]).
    // Queues and counters are untouched. Throws std::invalid_argument on a
    // negative burst.
    void set_max_burst(double max_burst_bytes);

    // Queues one packed payload. A keep-latest topic keeps only the newest
    // ([SES-26]); a queued topic past its bound drops the oldest and counts
    // it ([SES-30]). An unknown topic id is ignored. A keep-latest
    // replacement is dropped and counted too.
    void submit(std::uint8_t topic_id, std::vector<std::uint8_t> payload);

    // Sends what is due: at most one frame per topic, in handshake order from
    // after the topic that sent last ([SES-29]). A frame goes out only with a
    // per-topic permit and enough global bytes ([SES-27], [SES-28]). send
    // reports whether the frame left; on false the frame stays queued and
    // the pass stops ([SES-32]).
    template <typename Send> void poll(std::chrono::nanoseconds now, Send &&send)
    {
        if (entries_.empty()) {
            return;
        }
        accrue_budget(now);
        const std::size_t count = entries_.size();
        bool sent_any = false;
        std::size_t last_sent = cursor_;
        for (std::size_t step = 0; step < count; ++step) {
            const std::size_t at = (cursor_ + step) % count;
            Entry &entry = entries_[at];
            accrue_entry(entry, now);
            if (entry.queue.empty()) {
                continue;
            }
            if (entry.topic.rate_hz > 0.0 && entry.rate_tokens < 1.0) {
                continue;
            }
            const std::size_t bytes = encoded_frame_bound(entry.queue.front().size());
            if (budget_tokens_ < static_cast<double>(bytes)) {
                continue;
            }
            if (!send(entry.topic.topic_id, entry.queue.front())) {
                break;
            }
            if (entry.topic.rate_hz > 0.0) {
                entry.rate_tokens -= 1.0;
            }
            budget_tokens_ -= static_cast<double>(bytes);
            entry.queue.erase(entry.queue.begin());
            last_sent = at;
            sent_any = true;
        }
        if (sent_any) {
            cursor_ = (last_sent + 1u) % count;
        }
    }
    // Queued frames and dropped frames per topic, the latter for H8.
    std::size_t queued(std::uint8_t topic_id) const;
    std::size_t dropped(std::uint8_t topic_id) const;

    // The wire bytes one packed payload of packed_size bytes occupies: the
    // topic id, seq and CRC around it, the rCOBS code bytes and delimiter.
    static std::size_t encoded_frame_bound(std::size_t packed_size);

  private:
    struct Entry {
        SchedulerTopic topic;
        std::vector<std::vector<std::uint8_t>> queue;
        double rate_tokens = 0.0;
        std::chrono::nanoseconds last_seen{0};
        bool seen = false;
        std::size_t dropped = 0;
    };

    const Entry *find(std::uint8_t topic_id) const;
    Entry *find(std::uint8_t topic_id);
    void accrue_entry(Entry &entry, std::chrono::nanoseconds now);
    void accrue_budget(std::chrono::nanoseconds now);

    std::vector<Entry> entries_;
    std::size_t cursor_ = 0;
    double budget_bytes_per_sec_ = 0.0;
    double budget_capacity_ = 0.0;
    double budget_tokens_ = 0.0;
    std::chrono::nanoseconds budget_seen{0};
    bool budget_started_ = false;
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__SCHEDULER_HPP_
