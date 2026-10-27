/* posix port: the core's port contract over a serial device, with the tick
 * from CLOCK_MONOTONIC and the boot ID from a file counter (C10). */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"

#include "r2s_posix_port.h"

/* The variable naming the boot ID file. The test launching the firmware sets
 * it to a path it owns, so the counter never lives in the repository. */
#define BOOT_ENV "R2S_POSIX_BOOT_ID_FILE"

static int s_fd = -1;

int r2s_posix_open(const char *path)
{
    struct termios term;
    int fd = open(path, O_RDWR | O_NOCTTY);
    int flags;

    if (fd < 0) {
        return -1;
    }
    memset(&term, 0, sizeof term);
    term.c_cflag = (tcflag_t)(CS8 | CREAD | CLOCAL);
    term.c_cc[VMIN] = 0u;
    term.c_cc[VTIME] = 0u;
    if (cfsetispeed(&term, B115200) != 0 || cfsetospeed(&term, B115200) != 0 ||
        tcsetattr(fd, TCSANOW, &term) != 0) {
        int kept = errno;

        (void)close(fd);
        errno = kept;
        return -1;
    }
    flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        int kept = errno;

        (void)close(fd);
        errno = kept;
        return -1;
    }
    s_fd = fd;
    return fd;
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
    ssize_t n;

    if (s_fd < 0 || max == 0u) {
        return 0u;
    }
    n = read(s_fd, buf, max);
    if (n <= 0) {
        return 0u;
    }
    return (uint16_t)n;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
    uint16_t done = 0u;

    /* The port reports 0xFFFF free, so it may block: a full pty waits for the
     * reader instead of dropping the frame. */
    while (done < len) {
        ssize_t n = write(s_fd, buf + done, (size_t)(len - done));

        if (n < 0) {
            struct pollfd wait = {s_fd, POLLOUT, 0};

            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                fprintf(stderr, "r2s_posix: write failed: %s\n", strerror(errno));
                _exit(1);
            }
            (void)poll(&wait, 1u, -1);
        } else if (n == 0) {
            fprintf(stderr, "r2s_posix: write returned 0\n");
            _exit(1);
        } else {
            done = (uint16_t)(done + (uint16_t)n);
        }
    }
}

uint16_t r2s_port_tx_free(void)
{
    /* A blocking write accepts anything the core hands it (TX_FREE_BLOCKING),
     * so the core never sees TX_BUSY for lack of space on this port. */
    return 0xFFFFu;
}

uint32_t r2s_port_tick(void)
{
    /* Whole milliseconds since the first call. CLOCK_MONOTONIC never steps
     * back, and the truncation keeps the order, so the tick is monotonic. */
    static uint64_t base_ms;
    static uint8_t base_set;
    static uint32_t last;
    struct timespec now;
    uint64_t ms;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return last;
    }
    ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000L);
    if (base_set == 0u) {
        base_ms = ms;
        base_set = 1u;
    }
    last = (uint32_t)((ms - base_ms) * (uint32_t)(R2S_TICK_HZ / 1000u));
    return last;
}

/* Reads the file's decimal counter, adds one, writes it back and returns it.
 * A missing file starts at zero, so the first boot returns one. */
static uint32_t next_boot_id(const char *path)
{
    char text[32];
    char printed[32];
    char *end;
    unsigned long value;
    uint32_t id;
    int out;
    int fd = open(path, O_RDWR | O_CREAT, 0666);
    ssize_t n;

    if (fd < 0) {
        fprintf(stderr, "r2s_posix: cannot open boot ID file '%s': %s\n", path, strerror(errno));
        _exit(1);
    }
    n = read(fd, text, sizeof text - 1u);
    if (n < 0) {
        fprintf(stderr, "r2s_posix: cannot read boot ID file '%s': %s\n", path, strerror(errno));
        _exit(1);
    }
    if (n == 0) {
        value = 0ul;
    } else {
        text[(size_t)n] = '\0';
        errno = 0;
        value = strtoul(text, &end, 10);
        while (*end == '\n') {
            end++;
        }
        if (end == text || *end != '\0' || errno == ERANGE || value > 0xFFFFFFFFul) {
            fprintf(stderr, "r2s_posix: boot ID file '%s' is not a decimal u32\n", path);
            _exit(2);
        }
    }
    id = (uint32_t)(value + 1ul);
    out = snprintf(printed, sizeof printed, "%lu\n", (unsigned long)id);
    if (out < 0 || (size_t)out >= sizeof printed || lseek(fd, 0, SEEK_SET) != 0 || ftruncate(fd, 0) != 0 ||
        write(fd, printed, (size_t)out) != (ssize_t)out || close(fd) != 0) {
        fprintf(stderr, "r2s_posix: cannot write boot ID file '%s'\n", path);
        _exit(1);
    }
    return id;
}

uint32_t r2s_port_boot_id(void)
{
    static uint32_t cached;
    static uint8_t cached_set;

    if (cached_set == 0u) {
        const char *path = getenv(BOOT_ENV);

        if (path == NULL || path[0] == '\0') {
            fprintf(stderr, "r2s_posix: %s is not set\n", BOOT_ENV);
            _exit(2);
        }
        cached = next_boot_id(path);
        cached_set = 1u;
    }
    return cached;
}
