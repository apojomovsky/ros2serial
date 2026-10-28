/* unity.h for libr2s_host: bytes.c names UNITY_TEST_FAIL on its malformed hex
 * path. The driver never feeds one, so the hook records it like any other
 * fake port misuse, which the driver then raises. */
#ifndef R2S_HOST_UNITY_H
#define R2S_HOST_UNITY_H

#include "fake_port.h"

#define UNITY_TEST_FAIL(line, message)                                                                       \
    do {                                                                                                     \
        (void)(line);                                                                                        \
        fake_port_fail(message);                                                                             \
    } while (0)

#endif
