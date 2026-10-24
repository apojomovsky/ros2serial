// termios transport: an open tty in, non-blocking Transport calls out.
#include "r2s_bridge/termios_transport.hpp"

#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>
#include <string>

#if defined(__linux__)
#include <linux/serial.h>
#include <sys/ioctl.h>
#endif

namespace r2s_bridge
{
namespace
{

struct BaudCode {
    int rate;
    speed_t code;
};

constexpr BaudCode kBaudCodes[] = {
    {1200, B1200},       {2400, B2400},       {4800, B4800},       {9600, B9600},       {19200, B19200},
    {38400, B38400},     {57600, B57600},     {115200, B115200},   {230400, B230400},   {460800, B460800},
    {500000, B500000},   {576000, B576000},   {921600, B921600},   {1000000, B1000000}, {1152000, B1152000},
    {1500000, B1500000}, {2000000, B2000000}, {2500000, B2500000}, {3000000, B3000000}, {3500000, B3500000},
    {4000000, B4000000},
};

speed_t speed_for(int baud, const std::string &port)
{
    for (const BaudCode &entry : kBaudCodes) {
        if (entry.rate == baud) {
            return entry.code;
        }
    }
    throw std::invalid_argument("TermiosTransport: " + port + " has no termios constant for " +
                                std::to_string(baud) + " baud");
}

[[noreturn]] void throw_system_error(const std::string &what, const std::string &port)
{
    const int error = errno;
    throw std::runtime_error("TermiosTransport: " + what + " " + port + ": " + std::strerror(error));
}

#if defined(__linux__)
// Many USB adapters and every pty reject the request; the port still works at
// ordinary latency, so a refused ioctl is not an error.
void request_low_latency(int fd)
{
    struct serial_struct settings {
    };
    if (::ioctl(fd, TIOCGSERIAL, &settings) != 0) {
        return;
    }
    settings.flags |= ASYNC_LOW_LATENCY;
    (void)::ioctl(fd, TIOCSSERIAL, &settings);
}
#else
void request_low_latency(int)
{
}
#endif

} // namespace

TermiosTransport::TermiosTransport(const TermiosOptions &options)
{
    const speed_t speed = speed_for(options.baud, options.port);

    fd_ = ::open(options.port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
        throw_system_error("cannot open", options.port);
    }

    termios attributes{};
    if (tcgetattr(fd_, &attributes) != 0) {
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

    if (tcsetattr(fd_, TCSANOW, &attributes) != 0) {
        const int error = errno;
        ::close(fd_);
        fd_ = -1;
        errno = error;
        throw_system_error("cannot configure", options.port);
    }

    if (options.low_latency) {
        request_low_latency(fd_);
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
    for (;;) {
        const ssize_t count = ::read(fd_, buffer, capacity);
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
    for (;;) {
        const ssize_t count = ::write(fd_, data, size);
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

} // namespace r2s_bridge
