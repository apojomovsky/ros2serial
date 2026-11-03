#include <gtest/gtest.h>

#include <pty.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/serial.h>
#include <sys/ioctl.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
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

// A TermiosSyscalls that fails scripted calls with a scripted errno and
// runs the real syscall for everything else, so the transport still opens a
// real pty while chosen calls fail on demand.
class FaultySyscalls : public TermiosSyscalls
{
  public:
    int get_attributes_errno = 0;
    int set_attributes_errno = 0;
    bool serial_get_ok = false;
    mutable unsigned int serial_set_flags = 0;
    mutable int serial_set_count = 0;
    mutable std::size_t read_at = 0;
    std::vector<std::pair<ssize_t, int>> reads;
    mutable std::size_t write_at = 0;
    std::vector<std::pair<ssize_t, int>> writes;

    int get_attributes(int fd, termios *attributes) const noexcept override
    {
        if (get_attributes_errno != 0) {
            errno = get_attributes_errno;
            return -1;
        }
        return TermiosSyscalls::get_attributes(fd, attributes);
    }

    int set_attributes(int fd, int action, const termios *attributes) const noexcept override
    {
        if (set_attributes_errno != 0) {
            errno = set_attributes_errno;
            return -1;
        }
        return TermiosSyscalls::set_attributes(fd, action, attributes);
    }

    int serial_ioctl(int fd, unsigned long request, void *data) const noexcept override
    {
#if defined(__linux__)
        if (request == TIOCGSERIAL && serial_get_ok) {
            return 0;
        }
        if (request == TIOCSSERIAL) {
            ++serial_set_count;
            serial_set_flags = static_cast<struct serial_struct *>(data)->flags;
            return 0;
        }
#endif
        return TermiosSyscalls::serial_ioctl(fd, request, data);
    }

    ssize_t read_bytes(int fd, void *buffer, std::size_t capacity) const noexcept override
    {
        if (read_at < reads.size()) {
            const std::pair<ssize_t, int> outcome = reads[read_at++];
            if (outcome.first < 0) {
                errno = outcome.second;
            }
            return outcome.first;
        }
        return TermiosSyscalls::read_bytes(fd, buffer, capacity);
    }

    ssize_t write_bytes(int fd, const void *data, std::size_t size) const noexcept override
    {
        if (write_at < writes.size()) {
            const std::pair<ssize_t, int> outcome = writes[write_at++];
            if (outcome.first < 0) {
                errno = outcome.second;
            }
            return outcome.first;
        }
        return TermiosSyscalls::write_bytes(fd, data, size);
    }
};

TEST(TermiosTransport, ConfiguresThePortForRawNonBlockingIo)
{
    Pty pty;
    termios initial = AttributesOf(pty.slave());
    initial.c_iflag |= IXOFF | IXANY;
    initial.c_cflag |= CSTOPB;
    ASSERT_EQ(tcsetattr(pty.slave(), TCSANOW, &initial), 0);
    TermiosTransport transport(OptionsFor(pty));

    const termios attributes = AttributesOf(pty.slave());
    EXPECT_EQ(attributes.c_lflag & ICANON, 0u);
    EXPECT_EQ(attributes.c_lflag & ECHO, 0u);
    EXPECT_EQ(attributes.c_iflag & (IXON | IXOFF | IXANY), 0u);
    EXPECT_EQ(attributes.c_iflag & ICRNL, 0u);
    EXPECT_EQ(attributes.c_cflag & CSIZE, CS8);
    EXPECT_EQ(attributes.c_cflag & CSTOPB, 0u);
    EXPECT_EQ(attributes.c_cc[VMIN], 1);
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
    termios initial = AttributesOf(pty.slave());
    initial.c_cflag |= HUPCL;
    ASSERT_EQ(tcsetattr(pty.slave(), TCSANOW, &initial), 0);
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

// A pty refuses TIOCGSERIAL, so this covers only that the refused request
// leaves the port usable; the latency effect itself needs a real adapter.
TEST(TermiosTransport, KeepsAPortThatRejectsLowLatencyUsable)
{
    Pty pty;
#if defined(__linux__)
    struct serial_struct settings {
    };
    ASSERT_NE(::ioctl(pty.slave(), TIOCGSERIAL, &settings), 0)
        << "this test needs a pty that rejects TIOCGSERIAL";
#endif
    TermiosOptions options = OptionsFor(pty);
    options.low_latency = true;
    TermiosTransport transport(options);

    const std::uint8_t outgoing[] = {0x5a};
    ASSERT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{1, IoStatus::kOk}));

    std::uint8_t received[1]{};
    ASSERT_EQ(::read(pty.master(), received, sizeof received), 1);
    EXPECT_EQ(received[0], 0x5a);
}

TEST(TermiosTransport, KeepsAPortWithLowLatencyDisabledUsable)
{
    Pty pty;
    TermiosOptions options = OptionsFor(pty);
    options.low_latency = false;
    TermiosTransport transport(options);

    const std::uint8_t outgoing[] = {0x5a};
    ASSERT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{1, IoStatus::kOk}));

    std::uint8_t received[1]{};
    ASSERT_EQ(::read(pty.master(), received, sizeof received), 1);
    EXPECT_EQ(received[0], 0x5a);
}

TEST(TermiosTransport, ReportsUnreadablePortSettings)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.get_attributes_errno = ENOTTY;
    TermiosOptions options = OptionsFor(pty);

    try {
        TermiosTransport transport(options, syscalls);
        FAIL() << "settings that cannot be read must be reported";
    } catch (const std::runtime_error &error) {
        EXPECT_NE(std::string(error.what()).find("cannot read the settings of"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find(options.port), std::string::npos);
    }
}

TEST(TermiosTransport, ReadsSettingsFromACharacterDevice)
{
    TermiosOptions options;
    options.port = "/dev/null";

    try {
        TermiosTransport transport(options);
        FAIL() << "a device without settings must be reported";
    } catch (const std::runtime_error &error) {
        EXPECT_NE(std::string(error.what()).find("cannot read the settings of"), std::string::npos);
    }
}

TEST(TermiosTransport, RejectsAnUnconfigurablePort)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.set_attributes_errno = EINVAL;
    TermiosOptions options = OptionsFor(pty);

    try {
        TermiosTransport transport(options, syscalls);
        FAIL() << "settings that cannot be applied must be reported";
    } catch (const std::runtime_error &error) {
        EXPECT_NE(std::string(error.what()).find("cannot configure"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find(options.port), std::string::npos);
    }
}

#if defined(__linux__)
TEST(TermiosTransport, RequestsLowLatencyWhenTheDriverOffersIt)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.serial_get_ok = true;
    TermiosOptions options = OptionsFor(pty);
    TermiosTransport transport(options, syscalls);

    EXPECT_EQ(syscalls.serial_set_count, 1);
    EXPECT_NE(syscalls.serial_set_flags & static_cast<unsigned int>(ASYNC_LOW_LATENCY), 0u);

    const std::uint8_t outgoing[] = {0x5a};
    EXPECT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{1, IoStatus::kOk}));
}
#endif

TEST(TermiosTransport, ReadRetriesAfterAnInterruptedRead)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.reads.emplace_back(-1, EINTR);
    TermiosTransport transport(OptionsFor(pty), syscalls);
    const std::uint8_t outgoing[] = {0x01, 0x02};
    ASSERT_EQ(::write(pty.master(), outgoing, sizeof outgoing), static_cast<ssize_t>(sizeof outgoing));
    std::uint8_t buffer[8]{};

    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{2, IoStatus::kOk}));
    EXPECT_EQ(buffer[0], 0x01);
    EXPECT_EQ(buffer[1], 0x02);
}

TEST(TermiosTransport, ReportsAnIoErrorReadAsClosed)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.reads.emplace_back(-1, EIO);
    TermiosTransport transport(OptionsFor(pty), syscalls);
    std::uint8_t buffer[4]{};

    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kClosed}));
    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kClosed}));
}

TEST(TermiosTransport, ReportsAnUnknownReadErrnoAsAnError)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.reads.emplace_back(-1, EBADF);
    TermiosTransport transport(OptionsFor(pty), syscalls);
    std::uint8_t buffer[4]{};

    EXPECT_EQ(transport.read(buffer, sizeof buffer), (IoResult{0, IoStatus::kError}));
}

TEST(TermiosTransport, WriteRetriesAfterAnInterruptedWrite)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.writes.emplace_back(-1, EINTR);
    TermiosTransport transport(OptionsFor(pty), syscalls);
    const std::uint8_t outgoing[] = {0x11, 0x22};

    EXPECT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{2, IoStatus::kOk}));

    std::uint8_t received[8]{};
    ASSERT_EQ(::read(pty.master(), received, sizeof received), static_cast<ssize_t>(sizeof outgoing));
    EXPECT_EQ(received[0], 0x11);
    EXPECT_EQ(received[1], 0x22);
}

TEST(TermiosTransport, ReturnsWouldBlockWhenTheWriteAcceptsNothing)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.writes.emplace_back(0, 0);
    TermiosTransport transport(OptionsFor(pty), syscalls);
    const std::uint8_t outgoing[] = {0x11};

    EXPECT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{0, IoStatus::kWouldBlock}));
}

TEST(TermiosTransport, ReturnsWouldBlockOnALinkWithNoRoom)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.writes.emplace_back(-1, EAGAIN);
    TermiosTransport transport(OptionsFor(pty), syscalls);
    const std::uint8_t outgoing[] = {0x11};

    EXPECT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{0, IoStatus::kWouldBlock}));
}

TEST(TermiosTransport, ReportsAnIoErrorWriteAsClosed)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.writes.emplace_back(-1, EIO);
    TermiosTransport transport(OptionsFor(pty), syscalls);
    const std::uint8_t outgoing[] = {0x11};

    EXPECT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{0, IoStatus::kClosed}));
    EXPECT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{0, IoStatus::kClosed}));
}

TEST(TermiosTransport, ReportsAnUnknownWriteErrnoAsAnError)
{
    Pty pty;
    FaultySyscalls syscalls;
    syscalls.writes.emplace_back(-1, EBADF);
    TermiosTransport transport(OptionsFor(pty), syscalls);
    const std::uint8_t outgoing[] = {0x11};

    EXPECT_EQ(transport.write(outgoing, sizeof outgoing), (IoResult{0, IoStatus::kError}));
}

} // namespace
} // namespace r2s_bridge
