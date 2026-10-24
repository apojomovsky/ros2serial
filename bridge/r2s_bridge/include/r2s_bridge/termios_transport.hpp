// The production byte link: a POSIX termios port opened raw and non-blocking,
// with HUPCL and best-effort low-latency control. The session only ever sees
// the Transport interface, so the fake replaces this class wholesale.
#ifndef R2S_BRIDGE__TERMIOS_TRANSPORT_HPP_
#define R2S_BRIDGE__TERMIOS_TRANSPORT_HPP_

#include <cstddef>
#include <cstdint>
#include <string>

#include "r2s_bridge/transport.hpp"

namespace r2s_bridge
{

struct TermiosOptions {
    // Path to the serial device, for example /dev/serial/by-id/....
    std::string port;
    // Bits per second. Only rates with a termios constant are accepted; a
    // different one fails the constructor instead of silently picking another.
    int baud = 115200;
    // Keep HUPCL set, the adapter's default, or clear it so DTR does not drop
    // when the port closes, which avoids resetting a board on reopen.
    bool hupcl = true;
    // Ask the kernel for low-latency mode. Best effort: drivers that do not
    // implement it, including pty pairs, keep working without it.
    bool low_latency = true;
};

// Opens the port in the constructor and leaves it raw, non-blocking and
// eight-bit with no flow control. The constructor throws std::invalid_argument
// when baud has no termios constant, and std::runtime_error naming the port
// and the system error when it cannot be opened or configured.
class TermiosTransport final : public Transport
{
  public:
    explicit TermiosTransport(const TermiosOptions &options);

    // Closes the port unless close() already did.
    ~TermiosTransport() override;

    TermiosTransport(const TermiosTransport &) = delete;
    TermiosTransport &operator=(const TermiosTransport &) = delete;

    // The Transport contract, with kClosed terminal: once the peer hangs up or
    // close() runs, later reads and writes keep reporting kClosed.
    IoResult read(std::uint8_t *buffer, std::size_t capacity) override;
    IoResult write(const std::uint8_t *data, std::size_t size) override;
    void close() override;

  private:
    int fd_ = -1;
    bool closed_ = false;
};

} // namespace r2s_bridge

#endif // R2S_BRIDGE__TERMIOS_TRANSPORT_HPP_
