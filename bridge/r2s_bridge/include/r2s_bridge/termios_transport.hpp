// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

// The production byte link: a POSIX termios port opened raw and non-blocking,
// with HUPCL and best-effort low-latency control. The session only ever sees
// the Transport interface, so the fake replaces this class wholesale.
#ifndef R2S_BRIDGE__TERMIOS_TRANSPORT_HPP_
#define R2S_BRIDGE__TERMIOS_TRANSPORT_HPP_

#include <sys/types.h>
#include <termios.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "r2s_bridge/transport.hpp"

namespace r2s_bridge
{

/// The options opening the port reads.
/**
 * The production byte link reads a POSIX termios port raw and
 * non-blocking, with HUPCL and best-effort low-latency control.
 */
struct TermiosOptions
{
  /// Path to the serial device, for example /dev/serial/by-id/....
  std::string port;
  /// Bits per second.
  /**
   * Only rates with a termios constant are accepted; a different one
   * fails the constructor instead of silently picking another.
   */
  int baud = 115200;
  /// Keep HUPCL set, the adapter's default.
  /**
   * Clear it so DTR does not drop when the port closes, which avoids
   * resetting a board on reopen.
   */
  bool hupcl = true;
  /// Ask the kernel for low-latency mode.
  /**
   * Best effort: drivers that do not implement it, including pty pairs,
   * keep working without it.
   */
  bool low_latency = true;
};

/// The kernel calls the transport needs, so tests can fail each one on demand.
/**
 * The base runs the real syscalls; a test fake overrides some calls and
 * inherits the rest.
 */
class TermiosSyscalls
{
public:
  /// Destroys the syscall seam.
  virtual ~TermiosSyscalls() = default;

  /// Each call runs its syscall and returns what it returned.
  /**
   * Failures surface as the return value and errno, never as an
   * exception.
   *
   * \param[in] path the device path to open.
   * \param[in] flags the open flags.
   * \return The open file descriptor, or -1 on failure.
   */
  virtual int open_port(const char *path, int flags) const noexcept;

  /// Reads the port's termios attributes.
  /**
   * \param[in] fd the open port.
   * \param[out] attributes the attributes read.
   * \return 0 on success, -1 on failure.
   */
  virtual int get_attributes(int fd, termios *attributes) const noexcept;

  /// Writes the port's termios attributes.
  /**
   * \param[in] fd the open port.
   * \param[in] action when the change takes effect.
   * \param[in] attributes the attributes to write.
   * \return 0 on success, -1 on failure.
   */
  virtual int set_attributes(int fd, int action, const termios *attributes) const noexcept;

  /// Reads bytes from the port.
  /**
   * \param[in] fd the open port.
   * \param[out] buffer where to copy the bytes read.
   * \param[in] capacity how many bytes fit.
   * \return How many bytes were read, or -1 on failure.
   */
  virtual ssize_t read_bytes(int fd, void *buffer, std::size_t capacity) const noexcept;

  /// Writes bytes to the port.
  /**
   * \param[in] fd the open port.
   * \param[in] data the bytes to write.
   * \param[in] size how many bytes data holds.
   * \return How many bytes were written, or -1 on failure.
   */
  virtual ssize_t write_bytes(int fd, const void *data, std::size_t size) const noexcept;

  /// Runs an ioctl on the port.
  /**
   * The request mirrors the POSIX ::ioctl signature, so the seam keeps
   * unsigned long.
   *
   * \param[in] fd the open port.
   * \param[in] request the ioctl request.
   * \param[in] data the request payload.
   * \return 0 on success, -1 on failure.
   */
  // NOLINTNEXTLINE(runtime/int)
  virtual int serial_ioctl(int fd, unsigned long request, void *data) const noexcept;
};

/// Opens the port raw, non-blocking and eight-bit with no flow control.
/**
 * The constructor throws std::invalid_argument when baud has no termios
 * constant, and std::runtime_error naming the port and the system error
 * when it cannot be opened or configured.
 */
class TermiosTransport final : public Transport
{
public:
  /// Opens the port with the real syscalls.
  /**
   * \param[in] options the device path, baud and flags.
   * \throws std::invalid_argument when baud has no termios constant.
   * \throws std::runtime_error naming the port and the system error when
   * it cannot be opened or configured.
   */
  explicit TermiosTransport(const TermiosOptions & options);

  /// The same transport over injected syscalls.
  /**
   * Tests fail the port, baud and ioctl paths without hardware through it.
   *
   * \param[in] options the device path, baud and flags.
   * \param[in] syscalls the seam to run instead of the real syscalls.
   * \throws std::invalid_argument when baud has no termios constant.
   * \throws std::runtime_error naming the port and the system error when
   * it cannot be opened or configured.
   */
  TermiosTransport(const TermiosOptions & options, const TermiosSyscalls & syscalls);

  /// Closes the port unless close() already did.
  ~TermiosTransport() override;

  /// Not copyable.
  TermiosTransport(const TermiosTransport &) = delete;

  /// Not assignable.
  TermiosTransport & operator=(const TermiosTransport &) = delete;

  /// The Transport contract, with kClosed terminal.
  /**
   * Once the peer hangs up or close() runs, later reads and writes keep
   * reporting kClosed.
   *
   * \param[out] buffer where to copy the bytes read.
   * \param[in] capacity how many bytes fit.
   * \return The outcome: the bytes read, and why no more moved.
   */
  IoResult read(std::uint8_t *buffer, std::size_t capacity) override;

  /// The Transport contract, with kClosed terminal.
  /**
   * Once the peer hangs up or close() runs, later reads and writes keep
   * reporting kClosed.
   *
   * \param[in] data the bytes to write.
   * \param[in] size how many bytes data holds.
   * \return The outcome: the bytes accepted, and why no more moved.
   */
  IoResult write(const std::uint8_t *data, std::size_t size) override;

  /// Closes the port.
  /**
   * Later reads and writes report kClosed.
   */
  void close() override;

private:
  const TermiosSyscalls & syscalls_;
  int fd_ = -1;
  bool closed_ = false;
};

}  // namespace r2s_bridge

#endif  // R2S_BRIDGE__TERMIOS_TRANSPORT_HPP_
