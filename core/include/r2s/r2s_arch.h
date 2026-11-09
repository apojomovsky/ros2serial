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

/* arch: where constant data lives and how the core reads it back. On AVR
 * `const` alone leaves data in RAM, so tables carry R2S_FLASH (PROGMEM) and
 * string literals go through R2S_FSTR (PSTR); every other target reads plain
 * memory, and the macros pass data through unchanged. */
#ifndef R2S_ARCH_H
#define R2S_ARCH_H

#ifdef __AVR__
#include <avr/pgmspace.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Placement for constant tables (the codec descriptors, the topic table): the
 * PROGMEM attribute on AVR, empty elsewhere. A build that sets it first keeps
 * its value. */
#ifndef R2S_FLASH
#ifdef __AVR__
#define R2S_FLASH PROGMEM
#else
#define R2S_FLASH
#endif
#endif

/* Reads one byte of a table placed with R2S_FLASH. The codec and the topic
 * readers use it, so one generator order serves every target. */
#ifndef R2S_FLASH_READ_BYTE
#ifdef __AVR__
#define R2S_FLASH_READ_BYTE(p) pgm_read_byte(p)
#else
#define R2S_FLASH_READ_BYTE(p) (*(p))
#endif
#endif

/* Wraps a string literal so it stays in flash on AVR and passes it unchanged
 * elsewhere. The logging macros use it; other flash strings use it directly. */
#ifndef R2S_FSTR
#ifdef __AVR__
#define R2S_FSTR(lit) PSTR(lit)
#else
#define R2S_FSTR(lit) (lit)
#endif
#endif

/* A flash string: what the logging calls take. Read it with
 * R2S_FLASH_READ_BYTE, never with a plain dereference. */
typedef const char *r2s_fstr_t;

#ifdef __cplusplus
}
#endif

#endif
