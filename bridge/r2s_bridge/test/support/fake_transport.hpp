// A transport the tests script: reads come from a queue the test fills, writes
// are captured call by call. Limits: it never blocks, one thread drives it, and
// a scripted chunk is split across reads by the caller's capacity.
#ifndef R2S_BRIDGE__FAKE_TRANSPORT_HPP_
#define R2S_BRIDGE__FAKE_TRANSPORT_HPP_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

#include "r2s_bridge/transport.hpp"

namespace r2s_bridge
{
namespace test
{

class FakeTransport final : public Transport
{
  public:
    // Queues bytes the following reads return in order. An empty chunk is a
    // test bug and throws std::invalid_argument.
    void script_read(std::vector<std::uint8_t> bytes);

    // Queues a read outcome that carries no bytes. A read cannot report kOk
    // without data, so that status throws std::invalid_argument. kClosed is
    // terminal, as a hung-up link is.
    void script_read(IoStatus status);

    // Caps how many bytes each write accepts, modelling a port whose kernel
    // buffer is full. The default accepts every byte.
    void limit_write(std::size_t bytes);

    // Discards the queued reads. Reads and writes afterwards report kClosed.
    void close() override;

    IoResult read(std::uint8_t *buffer, std::size_t capacity) override;
    IoResult write(const std::uint8_t *data, std::size_t size) override;

    // The bytes of each accepted write call, in call order.
    const std::vector<std::vector<std::uint8_t>> &writes() const;
    bool closed() const;

  private:
    struct QueuedRead {
        std::vector<std::uint8_t> bytes;
        std::size_t offset = 0;
        IoStatus status = IoStatus::kOk;
    };

    std::deque<QueuedRead> script_;
    std::vector<std::vector<std::uint8_t>> writes_;
    std::size_t write_limit_ = std::numeric_limits<std::size_t>::max();
    bool closed_ = false;
};

} // namespace test
} // namespace r2s_bridge

#endif // R2S_BRIDGE__FAKE_TRANSPORT_HPP_
