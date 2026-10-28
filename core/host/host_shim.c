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
