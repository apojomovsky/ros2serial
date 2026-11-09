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

/* The stub Arduino core the glue's host test compiles against: a fake
 * Stream the test drives and a controllable micros clock. Only the calls
 * the glue makes exist here, so a new Arduino call fails to link. */
#ifndef ARDUINO_H
#define ARDUINO_H

#include <stddef.h>
#include <stdint.h>

#define HIGH 0x1
#define LOW 0x0

typedef bool boolean;
typedef uint8_t byte;

unsigned long micros(void);
void set_stub_micros(unsigned long value);

class Print
{
public:
  virtual size_t write(const uint8_t *buffer, size_t size) = 0;
  virtual int availableForWrite(void) = 0;
  virtual ~Print() = default;
};

class Stream : public Print
{
public:
  virtual int available(void) = 0;
  virtual int read(void) = 0;
  virtual ~Stream() = default;
};

/* A loopback stream: written bytes stay readable, so the test observes both
 * directions without a second peer. */
class StubStream : public Stream
{
public:
  StubStream(void);

  size_t write(const uint8_t *buffer, size_t size) override;
  int availableForWrite(void) override;
  int available(void) override;
  int read(void) override;
  void begin(unsigned long baud);
  void push(const uint8_t *data, size_t len);

  unsigned long begun_baud;
  bool begun;
  int tx_space;

private:
  static const size_t CAPACITY = 256;
  uint8_t m_buf[CAPACITY];
  size_t m_head;
  size_t m_tail;
  size_t m_count;
};

extern StubStream Serial;
extern StubStream Serial1;

#endif
