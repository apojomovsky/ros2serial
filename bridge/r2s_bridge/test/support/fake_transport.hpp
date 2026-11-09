// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

// A transport the tests script: reads come from a queue the test fills, writes
// are captured call by call. Limits: it never blocks, one thread drives it, and
// a scripted chunk is split across reads by the caller's capacity.
#ifndef SUPPORT__FAKE_TRANSPORT_HPP_
#define SUPPORT__FAKE_TRANSPORT_HPP_

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

    // Queues one write outcome the following writes return in order: a short
    // count models a port that takes some bytes and then reports WouldBlock,
    // an error models a port failing mid-frame. When the script runs out,
    // writes fall back to the limit above.
  void script_write(IoResult outcome);

    // Discards the queued reads. Reads and writes afterwards report kClosed.
  void close() override;

  IoResult read(std::uint8_t *buffer, std::size_t capacity) override;
  IoResult write(const std::uint8_t *data, std::size_t size) override;

    // The bytes of each accepted write call, in call order.
  const std::vector<std::vector<std::uint8_t>> & writes() const;
  bool closed() const;

    // Forgets every captured write, so a test can look at one step's output.
  void discard_writes();

private:
  struct QueuedRead
  {
    std::vector<std::uint8_t> bytes;
    std::size_t offset = 0;
    IoStatus status = IoStatus::kOk;
  };

  std::deque<QueuedRead> script_;
  std::deque<IoResult> write_script_;
  std::vector<std::vector<std::uint8_t>> writes_;
  std::size_t write_limit_ = std::numeric_limits<std::size_t>::max();
  bool closed_ = false;
};

}  // namespace test
}  // namespace r2s_bridge

#endif  // SUPPORT__FAKE_TRANSPORT_HPP_
