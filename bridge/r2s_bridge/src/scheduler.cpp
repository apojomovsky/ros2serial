// Host-to-MCU pacing (spec/protocol.md, 8.4). The bodies below are
// scaffolding for the tests-first commit; the pacing lands with the change.
#include "r2s_bridge/scheduler.hpp"

#include <stdexcept>

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
        entries_.push_back(entry);
    }
    cursor_ = 0;
    budget_bytes_per_sec_ = budget_bytes_per_sec;
    budget_tokens_ = 0.0;
    budget_started_ = false;
}

void Scheduler::submit(std::uint8_t, std::vector<std::uint8_t>)
{
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
