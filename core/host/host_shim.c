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

/* libr2s_host shim: the fake port's failure hook without Unity, the idle
 * cmd_vel handler, and the last-error cell the Python driver checks. */
#include <string.h>

#include "fake_port.h"
#include "handwritten.h"
#include "host_shim.h"
#include "r2s/r2s_codec.h"

static char host_error[160];

void fake_port_fail(const char *message)
{
    size_t taken = strlen(message);

    if (taken > sizeof host_error - 1u) {
        taken = sizeof host_error - 1u;
    }
    memcpy(host_error, message, taken);
    host_error[taken] = '\0';
}

const char *r2s_host_error(void)
{
    return host_error;
}

/* Dispatch calls the handler on every valid cmd_vel frame; the model tests
 * observe the frame through TX and the counters, so the handler keeps
 * nothing. */
void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
}

/* Dispatch calls the handler on every valid cmd_str frame; like cmd_vel it
 * keeps nothing the model tests would observe. */
void r2s_on_cmd_str(const std_msgs__msg__String *msg)
{
    (void)msg;
}
