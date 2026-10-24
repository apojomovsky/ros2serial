#include "fake_transport.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace r2s_bridge
{
namespace test
{

void FakeTransport::script_read(std::vector<std::uint8_t> bytes)
{
    if (bytes.empty()) {
        throw std::invalid_argument("FakeTransport::script_read needs at least one byte");
    }
    QueuedRead queued;
    queued.bytes = std::move(bytes);
    script_.push_back(std::move(queued));
}

void FakeTransport::script_read(IoStatus status)
{
    if (status == IoStatus::kOk) {
        throw std::invalid_argument("FakeTransport::script_read(kOk) needs bytes");
    }
    QueuedRead queued;
    queued.status = status;
    script_.push_back(std::move(queued));
}

void FakeTransport::limit_write(std::size_t bytes)
{
    write_limit_ = bytes;
}

void FakeTransport::close()
{
    closed_ = true;
    script_.clear();
}

IoResult FakeTransport::read(std::uint8_t *buffer, std::size_t capacity)
{
    if (closed_) {
        return {0, IoStatus::kClosed};
    }
    if (capacity == 0) {
        throw std::invalid_argument("FakeTransport::read needs a capacity of at least one");
    }
    if (script_.empty()) {
        return {0, IoStatus::kWouldBlock};
    }
    QueuedRead &head = script_.front();
    if (head.status != IoStatus::kOk) {
        const IoStatus status = head.status;
        script_.pop_front();
        if (status == IoStatus::kClosed) {
            closed_ = true;
            script_.clear();
        }
        return {0, status};
    }
    const std::size_t count = std::min(capacity, head.bytes.size() - head.offset);
    if (count == 0) {
        return {0, IoStatus::kWouldBlock};
    }
    std::copy_n(head.bytes.begin() + static_cast<std::ptrdiff_t>(head.offset), count, buffer);
    head.offset += count;
    if (head.offset == head.bytes.size()) {
        script_.pop_front();
    }
    return {count, IoStatus::kOk};
}

IoResult FakeTransport::write(const std::uint8_t *data, std::size_t size)
{
    if (closed_) {
        return {0, IoStatus::kClosed};
    }
    if (size == 0) {
        throw std::invalid_argument("FakeTransport::write needs at least one byte");
    }
    const std::size_t count = std::min(size, write_limit_);
    if (count == 0) {
        return {0, IoStatus::kWouldBlock};
    }
    writes_.emplace_back(data, data + count);
    return {count, IoStatus::kOk};
}

const std::vector<std::vector<std::uint8_t>> &FakeTransport::writes() const
{
    return writes_;
}

bool FakeTransport::closed() const
{
    return closed_;
}

} // namespace test
} // namespace r2s_bridge
