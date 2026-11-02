/* Opening the serial line: the firmware main's handle to the port. */
#ifndef R2S_POSIX_PORT_H
#define R2S_POSIX_PORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opens path as a raw 115200 8-N-1 line with non-blocking reads. Returns the
 * file descriptor the port reads and writes, or -1 with errno set, so the
 * caller reports the path it could not open. */
int r2s_posix_open(const char *path);

/* Skew applied to the tick in parts per million, 0 for a true clock. A
 * positive skew runs the MCU fast, so the host fits a drift it must then
 * correct. Firmware sets it from --tick-skew-ppm before opening the port;
 * values past +-20000 ppm leave the correction range the wire allows. */
void r2s_posix_set_tick_skew_ppm(int32_t ppm);

#ifdef __cplusplus
}
#endif

#endif
