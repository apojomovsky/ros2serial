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

/* The stub Arduino core behind the glue's host test. */
#include "Arduino.h"

static unsigned long s_micros;

unsigned long micros(void)
{
    return s_micros;
}

void set_stub_micros(unsigned long value)
{
    s_micros = value;
}

StubStream::StubStream(void) : begun_baud(0), begun(false), tx_space(64), m_head(0), m_tail(0), m_count(0)
{
}

size_t StubStream::write(const uint8_t *buffer, size_t size)
{
    size_t i;

    for (i = 0; i < size && m_count < CAPACITY; i++) {
        m_buf[m_tail] = buffer[i];
        m_tail = (m_tail + 1) % CAPACITY;
        m_count++;
    }
    return i;
}

int StubStream::availableForWrite(void)
{
    return tx_space;
}

int StubStream::available(void)
{
    return (int)m_count;
}

int StubStream::read(void)
{
    uint8_t next;

    if (m_count == 0) {
        return -1;
    }
    next = m_buf[m_head];
    m_head = (m_head + 1) % CAPACITY;
    m_count--;
    return next;
}

void StubStream::begin(unsigned long baud)
{
    begun = true;
    begun_baud = baud;
}

void StubStream::push(const uint8_t *data, size_t len)
{
    (void)write(data, len);
}

StubStream Serial;
StubStream Serial1;
