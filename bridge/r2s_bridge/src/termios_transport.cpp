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

// termios transport: an open tty in, non-blocking Transport calls out.
#include "r2s_bridge/termios_transport.hpp"

#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>
#if defined(__linux__)
#include <linux/serial.h>
#endif

namespace r2s_bridge
{

int TermiosSyscalls::open_port(const char *path, int flags) const noexcept
{
  return ::open(path, flags);
}

int TermiosSyscalls::get_attributes(int fd, termios *attributes) const noexcept
{
  return ::tcgetattr(fd, attributes);
}

int TermiosSyscalls::set_attributes(int fd, int action, const termios *attributes) const noexcept
{
  return ::tcsetattr(fd, action, attributes);
}

ssize_t TermiosSyscalls::read_bytes(int fd, void *buffer, std::size_t capacity) const noexcept
{
  return ::read(fd, buffer, capacity);
}

ssize_t TermiosSyscalls::write_bytes(int fd, const void *data, std::size_t size) const noexcept
{
  return ::write(fd, data, size);
}

// The request mirrors the POSIX ::ioctl signature, so the seam keeps unsigned long.
// NOLINTNEXTLINE(runtime/int)
int TermiosSyscalls::serial_ioctl(int fd, unsigned long request, void *data) const noexcept
{
  return ::ioctl(fd, request, data);
}

namespace
{

struct BaudCode
{
  int rate;
  speed_t code;
};

constexpr BaudCode kBaudCodes[] = {
  {1200, B1200}, {2400, B2400}, {4800, B4800}, {9600, B9600}, {19200, B19200},
  {38400, B38400}, {57600, B57600}, {115200, B115200}, {230400, B230400}, {460800, B460800},
  {500000, B500000}, {576000, B576000}, {921600, B921600}, {1000000, B1000000}, {1152000, B1152000},
  {1500000, B1500000}, {2000000, B2000000}, {2500000, B2500000}, {3000000, B3000000},
  {3500000, B3500000},
  {4000000, B4000000},
};

speed_t speed_for(int baud, const std::string & port)
{
  for (const BaudCode & entry : kBaudCodes) {
    if (entry.rate == baud) {
      return entry.code;
    }
  }
  throw std::invalid_argument("TermiosTransport: " + port + " has no termios constant for " +
                                std::to_string(baud) + " baud");
}

[[noreturn]] void throw_system_error(const std::string & what, const std::string & port)
{
  const int error = errno;
  throw std::runtime_error("TermiosTransport: " + what + " " + port + ": " + std::strerror(error));
}

#if defined(__linux__)
// Many USB adapters and every pty reject the request; the port still works at
// ordinary latency, so a refused ioctl is not an error.
void request_low_latency(int fd, const TermiosSyscalls & syscalls)
{
  struct serial_struct settings
  {
  };
  if (syscalls.serial_ioctl(fd, TIOCGSERIAL, &settings) != 0) {
    return;
  }
  settings.flags |= ASYNC_LOW_LATENCY;
  (void)syscalls.serial_ioctl(fd, TIOCSSERIAL, &settings);
}
#else
void request_low_latency(int, const TermiosSyscalls &)
{
}
#endif

const TermiosSyscalls & real_syscalls()
{
  static const TermiosSyscalls instance;
  return instance;
}

}  // namespace

TermiosTransport::TermiosTransport(const TermiosOptions & options)
: TermiosTransport(options, real_syscalls())
{
}

TermiosTransport::TermiosTransport(const TermiosOptions & options, const TermiosSyscalls & syscalls)
: syscalls_(syscalls)
{
  const speed_t speed = speed_for(options.baud, options.port);

  fd_ = syscalls_.open_port(options.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    throw_system_error("cannot open", options.port);
  }

  termios attributes{};
  if (syscalls_.get_attributes(fd_, &attributes) != 0) {
    const int error = errno;
    ::close(fd_);
    fd_ = -1;
    errno = error;
    throw_system_error("cannot read the settings of", options.port);
  }

  cfmakeraw(&attributes);
  attributes.c_cflag |= CLOCAL | CREAD;
#ifdef CRTSCTS
  attributes.c_cflag &= ~static_cast<tcflag_t>(CRTSCTS);
#endif
  attributes.c_cflag &= ~static_cast<tcflag_t>(CSTOPB);
    // cfmakeraw clears IXON only, so a port arriving with IXOFF set would keep
    // emitting XOFF toward the MCU.
  attributes.c_iflag &= ~static_cast<tcflag_t>(IXON | IXOFF | IXANY);
    // VMIN 1 with O_NONBLOCK reports EAGAIN on an empty queue; VMIN 0 returns
    // zero bytes instead, which is indistinguishable from a hangup.
  attributes.c_cc[VMIN] = 1;
  attributes.c_cc[VTIME] = 0;
  if (options.hupcl) {
    attributes.c_cflag |= HUPCL;
  } else {
    attributes.c_cflag &= ~static_cast<tcflag_t>(HUPCL);
  }
  cfsetispeed(&attributes, speed);
  cfsetospeed(&attributes, speed);

  if (syscalls_.set_attributes(fd_, TCSANOW, &attributes) != 0) {
    const int error = errno;
    ::close(fd_);
    fd_ = -1;
    errno = error;
    throw_system_error("cannot configure", options.port);
  }

  if (options.low_latency) {
    request_low_latency(fd_, syscalls_);
  }
}

TermiosTransport::~TermiosTransport()
{
  close();
}

IoResult TermiosTransport::read(std::uint8_t *buffer, std::size_t capacity)
{
  if (closed_ || fd_ < 0) {
    return {0, IoStatus::kClosed};
  }
  if (capacity == 0) {
    throw std::invalid_argument("TermiosTransport::read needs a capacity of at least one");
  }
  for (;; ) {
    const ssize_t count = syscalls_.read_bytes(fd_, buffer, capacity);
    if (count > 0) {
      return {static_cast<std::size_t>(count), IoStatus::kOk};
    }
    if (count == 0) {
      closed_ = true;
      return {0, IoStatus::kClosed};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return {0, IoStatus::kWouldBlock};
    }
        // A hung-up tty reports EIO rather than EOF on Linux.
    if (errno == EIO) {
      closed_ = true;
      return {0, IoStatus::kClosed};
    }
    return {0, IoStatus::kError};
  }
}

IoResult TermiosTransport::write(const std::uint8_t *data, std::size_t size)
{
  if (closed_ || fd_ < 0) {
    return {0, IoStatus::kClosed};
  }
  if (size == 0) {
    throw std::invalid_argument("TermiosTransport::write needs at least one byte");
  }
  for (;; ) {
    const ssize_t count = syscalls_.write_bytes(fd_, data, size);
    if (count > 0) {
      return {static_cast<std::size_t>(count), IoStatus::kOk};
    }
    if (count == 0) {
      return {0, IoStatus::kWouldBlock};
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return {0, IoStatus::kWouldBlock};
    }
    if (errno == EIO || errno == EPIPE) {
      closed_ = true;
      return {0, IoStatus::kClosed};
    }
    return {0, IoStatus::kError};
  }
}

void TermiosTransport::close()
{
  closed_ = true;
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

}  // namespace r2s_bridge
