#include <gtest/gtest.h>

#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "r2s_bridge/termios_transport.hpp"

namespace r2s_bridge
{
namespace
{

// A pseudo terminal stands in for a serial adapter: the kernel applies the same
// tty settings to it, so these tests never need a device.
class Pty
{
  public:
    Pty()
    {
        if (openpty(&master_, &slave_, nullptr, nullptr, nullptr) != 0) {
            throw std::runtime_error(std::string("openpty failed: ") + std::strerror(errno));
        }
        char name[256]{};
        if (ptsname_r(master_, name, sizeof name) != 0) {
            throw std::runtime_error(std::string("ptsname_r failed: ") + std::strerror(errno));
        }
        name_ = name;
    }

    ~Pty()
    {
        close_master();
        if (slave_ >= 0) {
            ::close(slave_);
        }
    }

    Pty(const Pty &) = delete;
    Pty &operator=(const Pty &) = delete;

    const std::string &name() const
    {
        return name_;
    }
    int master() const
    {
        return master_;
    }
    int slave() const
    {
        return slave_;
    }

    void close_master()
    {
        if (master_ >= 0) {
            ::close(master_);
            master_ = -1;
        }
    }

  private:
    int master_ = -1;
    int slave_ = -1;
    std::string name_;
};

TermiosOptions OptionsFor(const Pty &pty)
{
    TermiosOptions options;
    options.port = pty.name();
    return options;
}

termios AttributesOf(int fd)
{
    termios attributes{};
    if (tcgetattr(fd, &attributes) != 0) {
        throw std::runtime_error(std::string("tcgetattr failed: ") + std::strerror(errno));
    }
    return attributes;
}

TEST(TermiosTransport, ConfiguresThePortForRawNonBlockingIo)
{
    Pty pty;
    TermiosTransport transport(OptionsFor(pty));

    const termios attributes = AttributesOf(pty.slave());
    EXPECT_EQ(attributes.c_lflag & ICANON, 0u);
    EXPECT_EQ(attributes.c_lflag & ECHO, 0u);
    EXPECT_EQ(attributes.c_iflag & IXON, 0u);
    EXPECT_EQ(attributes.c_iflag & ICRNL, 0u);
    EXPECT_EQ(attributes.c_cflag & CSIZE, CS8);
    EXPECT_EQ(attributes.c_cc[VMIN], 0);
    EXPECT_EQ(attributes.c_cc[VTIME], 0);
}

TEST(TermiosTransport, AppliesTheBaudRate)
{
    Pty pty;
    TermiosOptions options = OptionsFor(pty);
    options.baud = 115200;
    TermiosTransport transport(options);

    termios attributes = AttributesOf(pty.slave());
    EXPECT_EQ(cfgetospeed(&attributes), B115200);
    EXPECT_EQ(cfgetispeed(&attributes), B115200);
}

TEST(TermiosTransport, SetsHupclWhenRequested)
{
    Pty pty;
    TermiosOptions options = OptionsFor(pty);
    options.hupcl = true;
    TermiosTransport transport(options);

    const termios attributes = AttributesOf(pty.slave());
    EXPECT_NE(attributes.c_cflag & HUPCL, 0u);
}

TEST(TermiosTransport, ClearsHupclWhenDisabled)
{
    Pty pty;
    TermiosOptions options = OptionsFor(pty);
    options.hupcl = false;
    TermiosTransport transport(options);

    const termios attributes = AttributesOf(pty.slave());
    EXPECT_EQ(attributes.c_cflag & HUPCL, 0u);
}

TEST(TermiosTransport, ReadsWhatThePeerWrites)
{
    Pty pty;
    TermiosTransport transport(OptionsFor(pty));
    const std::uint8_t outgoing[] = {0x01, 0x02, 0x03};
    ASSERT_EQ(::write(pty.master(), outgoing, sizeof outgoing), static_cast<ssize_t>(sizeof outgoing));
    std::uint8_t buffer[8]{};

    const IoResult result = transport.read(buffer, sizeof buffer);

    ASSERT_EQ(result, (IoResult{3, IoStatus::kOk}));
    EXPECT_EQ(std::vector<std::uint8_t>(buffer, buffer + result.bytes),
              (std::vector<std::uint8_t>{0x01, 0x02, 0x03}));
}

TEST(TermiosTransport, SplitsAReadAtTheCapacity)
{
    Pty pty;
    TermiosTransport transport(OptionsFor(pty));
    const std::uint8_t outgoing[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    ASSERT_EQ(::write(pty.master(), outgoing, sizeof outgoing), static_cast<ssize_t>(sizeof outgoing));
    std::uint8_t buffer[2]{};

    const IoResult first = transport.read(buffer, sizeof buffer);
    ASSERT_EQ(first, (IoResult{2, IoStatus::kOk}));
    EXPECT_EQ(std::vector<std::uint8_t>(buffer, buffer + first.bytes),
              (std::vector<std::uint8_t>{0x01, 0x02}));

    const IoResult second = transport.read(buffer, sizeof buffer);
    ASSERT_EQ(second, (IoResult{2, IoStatus::kOk}));
    EXPECT_EQ(std::vector<std::uint8_t>(buffer, buffer + second.bytes),
              (std::vector<std::uint8_t>{0x03, 0x04}));
}

TEST(TermiosTransport, ReportsWouldBlockWhenTheLinkHasNoData)
{
    Pty pty;
    TermiosTransport transport(OptionsFor(pty));
    std::uint8_t buffer[4]{};

    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kWouldBlock}));
}

TEST(TermiosTransport, WritesWhatThePeerReads)
{
    Pty pty;
    TermiosTransport transport(OptionsFor(pty));
    const std::uint8_t outgoing[] = {0x11, 0x22, 0x33};

    EXPECT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{3, IoStatus::kOk}));

    std::uint8_t received[8]{};
    ASSERT_EQ(::read(pty.master(), received, sizeof received), static_cast<ssize_t>(sizeof outgoing));
    EXPECT_EQ(std::vector<std::uint8_t>(received, received + sizeof outgoing),
              (std::vector<std::uint8_t>{0x11, 0x22, 0x33}));
}

TEST(TermiosTransport, ReportsClosedAfterThePeerHangsUp)
{
    Pty pty;
    TermiosTransport transport(OptionsFor(pty));
    std::uint8_t buffer[4]{};
    pty.close_master();

    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kClosed}));
    EXPECT_EQ(transport.write(buffer, 1), (IoResult{0, IoStatus::kClosed}));
}

TEST(TermiosTransport, CloseEndsTheLinkAndIsIdempotent)
{
    Pty pty;
    TermiosTransport transport(OptionsFor(pty));
    std::uint8_t buffer[4]{};

    transport.close();

    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kClosed}));
    EXPECT_EQ(transport.write(buffer, 1), (IoResult{0, IoStatus::kClosed}));
    transport.close();
}

TEST(TermiosTransport, RejectsAZeroCapacityRead)
{
    Pty pty;
    TermiosTransport transport(OptionsFor(pty));
    std::uint8_t buffer[4]{};

    EXPECT_THROW(transport.read(buffer, 0), std::invalid_argument);
}

TEST(TermiosTransport, RejectsAZeroLengthWrite)
{
    Pty pty;
    TermiosTransport transport(OptionsFor(pty));
    const std::uint8_t buffer[1]{};

    EXPECT_THROW(transport.write(buffer, 0), std::invalid_argument);
}

TEST(TermiosTransport, RejectsAnUnsupportedBaudRate)
{
    Pty pty;
    TermiosOptions options = OptionsFor(pty);
    options.baud = 12345;

    try {
        TermiosTransport transport(options);
        FAIL() << "a baud rate without a termios constant must be rejected";
    } catch (const std::invalid_argument &error) {
        EXPECT_NE(std::string(error.what()).find("12345"), std::string::npos);
    }
}

TEST(TermiosTransport, RejectsAPortThatCannotBeOpened)
{
    TermiosOptions options;
    options.port = "/nonexistent/r2s-test-port";

    try {
        TermiosTransport transport(options);
        FAIL() << "a port that cannot be opened must be reported";
    } catch (const std::runtime_error &error) {
        EXPECT_NE(std::string(error.what()).find(options.port), std::string::npos);
    }
}

TEST(TermiosTransport, RequestsLowLatencyOnAPortThatRejectsIt)
{
    Pty pty;
    TermiosOptions options = OptionsFor(pty);
    options.low_latency = true;

    TermiosTransport transport(options);

    std::uint8_t buffer[1]{};
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kWouldBlock}));
}

} // namespace
} // namespace r2s_bridge
