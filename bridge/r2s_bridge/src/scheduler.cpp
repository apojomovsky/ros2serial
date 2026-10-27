// Host-to-MCU pacing (spec/protocol.md, 8.4).
#include "r2s_bridge/scheduler.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace r2s_bridge
{

void Scheduler::configure(std::vector<SchedulerTopic> topics, double budget_bytes_per_sec)
{
    if (budget_bytes_per_sec < 0.0) {
        throw std::invalid_argument("r2s_bridge::Scheduler: the budget must not be negative");
    }
    entries_.clear();
    for (const SchedulerTopic &topic : topics) {
        if (topic.rate_hz < 0.0) {
            throw std::invalid_argument("r2s_bridge::Scheduler: the rate must not be negative");
        }
        Entry entry;
        entry.topic = topic;
        // Buckets start full, so a first frame never waits for tokens.
        entry.rate_tokens = 1.0;
        entries_.push_back(entry);
    }
    cursor_ = 0;
    budget_bytes_per_sec_ = budget_bytes_per_sec;
    budget_tokens_ = budget_bytes_per_sec;
    budget_started_ = false;
}

void Scheduler::submit(std::uint8_t topic_id, std::vector<std::uint8_t> payload)
{
    Entry *entry = find(topic_id);
    if (entry == nullptr) {
        return;
    }
    if (entry->topic.keep_latest) {
        entry->queue.clear();
        entry->queue.push_back(std::move(payload));
        return;
    }
    const std::size_t bound = entry->topic.queue_bound < 1u ? 1u : entry->topic.queue_bound;
    while (entry->queue.size() >= bound) {
        entry->queue.erase(entry->queue.begin());
        ++entry->dropped;
    }
    entry->queue.push_back(std::move(payload));
}

void Scheduler::accrue_entry(Entry &entry, std::chrono::nanoseconds now)
{
    if (entry.topic.rate_hz <= 0.0) {
        return;
    }
    if (!entry.seen) {
        entry.seen = true;
        entry.last_seen = now;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - entry.last_seen).count();
    entry.last_seen = now;
    entry.rate_tokens = std::min(1.0, entry.rate_tokens + elapsed * entry.topic.rate_hz);
}

void Scheduler::accrue_budget(std::chrono::nanoseconds now)
{
    if (!budget_started_) {
        budget_started_ = true;
        budget_seen = now;
        return;
    }
    const double elapsed = std::chrono::duration<double>(now - budget_seen).count();
    budget_seen = now;
    budget_tokens_ = std::min(budget_bytes_per_sec_, budget_tokens_ + elapsed * budget_bytes_per_sec_);
}
std::size_t Scheduler::queued(std::uint8_t topic_id) const
{
    const Entry *entry = find(topic_id);
    return entry == nullptr ? 0u : entry->queue.size();
}

std::size_t Scheduler::dropped(std::uint8_t topic_id) const
{
    const Entry *entry = find(topic_id);
    return entry == nullptr ? 0u : entry->dropped;
}

std::size_t Scheduler::encoded_frame_bound(std::size_t packed_size)
{
    const std::size_t decoded = packed_size + 4u;
    return decoded + decoded / 254u + 2u;
}

const Scheduler::Entry *Scheduler::find(std::uint8_t topic_id) const
{
    for (const Entry &entry : entries_) {
        if (entry.topic.topic_id == topic_id) {
            return &entry;
        }
    }
    return nullptr;
}

Scheduler::Entry *Scheduler::find(std::uint8_t topic_id)
{
    for (Entry &entry : entries_) {
        if (entry.topic.topic_id == topic_id) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace r2s_bridge
