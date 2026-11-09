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

/* posix port: the core's port contract over a serial device, with the tick
 * from CLOCK_MONOTONIC. */
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
    tcsetattr(fd, TCSANOW, &term) != 0)
  {
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

/* What r2s_port_tx_free() reports when no cap applies. */
#define TX_FREE_UNCAPPED 0xFFFFu

/* The saturation budget: bytes still writable before the tick moves. */
static uint16_t s_tx_cap = TX_FREE_UNCAPPED;
static uint16_t s_tx_left;
static uint32_t s_tx_tick;

void r2s_posix_set_tx_cap(uint16_t cap)
{
  s_tx_cap = cap;
  s_tx_left = cap;
  s_tx_tick = r2s_port_tick();
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t done = 0u;

  if (s_tx_cap != TX_FREE_UNCAPPED) {
        /* The core reserved these bytes from the budget at begin, so the
         * budget still holds them; the floor below is belt and braces. */
    if (s_tx_left >= len) {
      s_tx_left = (uint16_t)(s_tx_left - len);
    } else {
      s_tx_left = 0u;
    }
  }
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
  uint32_t now;

  if (s_tx_cap == TX_FREE_UNCAPPED) {
        /* A blocking write accepts anything the core hands it
         * (TX_FREE_BLOCKING), so the core never sees TX_BUSY for lack of
         * space on this port. */
    return TX_FREE_UNCAPPED;
  }
    /* The budget refills when the tick moves, so one tick's flood cannot
     * borrow the next tick's bytes. */
  now = r2s_port_tick();
  if (now != s_tx_tick) {
    s_tx_tick = now;
    s_tx_left = s_tx_cap;
  }
  return s_tx_left;
}

/* The tick skew in parts per million, set once before the port opens. */
static int32_t s_skew_ppm;

void r2s_posix_set_tick_skew_ppm(int32_t ppm)
{
  s_skew_ppm = ppm;
}

uint32_t r2s_port_tick(void)
{
    /* Whole milliseconds since the first call, scaled by the skew knob.
     * CLOCK_MONOTONIC never steps back, and the truncation keeps the order,
     * so the tick is monotonic. The scale stays in 64 bits: the product of
     * a millisecond count with a factor near one cannot overflow. */
  static uint64_t base_ms;
  static uint8_t base_set;
  static uint32_t last;
  struct timespec now;
  uint64_t ms;
  uint64_t elapsed;

  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
    return last;
  }
  ms = (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000L);
  if (base_set == 0u) {
    base_ms = ms;
    base_set = 1u;
  }
  elapsed = ms - base_ms;
  last = (uint32_t)(((elapsed * (uint64_t)(1000000 + s_skew_ppm)) / 1000000u) *
    (uint32_t)(R2S_TICK_HZ / 1000u));
  return last;
}
