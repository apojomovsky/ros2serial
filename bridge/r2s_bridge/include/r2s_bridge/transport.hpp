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

// The byte link the bridge talks through: an open serial port in production, a
// scripted fake in tests. Reads never block, so the caller owns the waiting, and
// nothing below this interface touches the port directly.
#ifndef R2S_BRIDGE__TRANSPORT_HPP_
#define R2S_BRIDGE__TRANSPORT_HPP_

#include <cstddef>
#include <cstdint>

namespace r2s_bridge
{

/// The outcome of one read or write.
enum class IoStatus : std::uint8_t
{
  kOk,  ///< The call moved at least one byte.
  kWouldBlock,  ///< The call moved nothing; the caller retries later.
  kClosed,  ///< The peer hung up.
  kError,  ///< The call failed.
};

/// The outcome of one read or write: bytes moved, and why no more moved.
/**
 * kOk means at least one byte moved.
 */
struct IoResult
{
  std::size_t bytes = 0;  ///< How many bytes moved.
  IoStatus status = IoStatus::kOk;  ///< Why no more moved.
};

/// Two outcomes compare equal when both fields match.
/**
 * \param[in] lhs the left outcome.
 * \param[in] rhs the right outcome.
 * \return True when both fields match.
 */
constexpr bool operator==(const IoResult & lhs, const IoResult & rhs)
{
  return lhs.bytes == rhs.bytes && lhs.status == rhs.status;
}

/// Two outcomes compare unequal when any field differs.
/**
 * \param[in] lhs the left outcome.
 * \param[in] rhs the right outcome.
 * \return True when any field differs.
 */
constexpr bool operator!=(const IoResult & lhs, const IoResult & rhs)
{
  return !(lhs == rhs);
}

/// The byte link the bridge talks through.
/**
 * An open serial port in production, a scripted fake in tests. Reads never
 * block, so the caller owns the waiting, and nothing below this interface
 * touches the port directly.
 */
class Transport
{
public:
  /// Destroys the link.
  virtual ~Transport() = default;

  /// Reads at most capacity bytes into buffer and never blocks.
  /**
   * capacity is at least one. Returns kOk with the bytes read when the link
   * had data, kWouldBlock when it had none, kClosed after the peer hung up,
   * and kError when the read failed.
   *
   * \param[out] buffer where to copy the bytes read.
   * \param[in] capacity how many bytes fit.
   * \return The outcome: the bytes read, and why no more moved.
   */
  virtual IoResult read(std::uint8_t *buffer, std::size_t capacity) = 0;

  /// Writes size bytes and returns how many the link accepted.
  /**
   * size is at least one; the accepted count may be fewer. kWouldBlock
   * means it accepted nothing this time; the caller keeps the rest for
   * the next write.
   *
   * \param[in] data the bytes to write.
   * \param[in] size how many bytes data holds.
   * \return The outcome: the bytes accepted, and why no more moved.
   */
  virtual IoResult write(const std::uint8_t *data, std::size_t size) = 0;

  /// Closes the link.
  /**
   * Reads and writes afterwards report kClosed.
   */
  virtual void close() = 0;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__TRANSPORT_HPP_
