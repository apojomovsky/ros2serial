/* Opening the serial line: the firmware main's handle to the port. */
#ifndef R2S_POSIX_PORT_H
#define R2S_POSIX_PORT_H

#ifdef __cplusplus
extern "C" {
#endif
/* Opens path as a raw 115200 8-N-1 line with non-blocking reads. Returns the
 * file descriptor the port reads and writes, or -1 with errno set, so the
 * caller reports the path it could not open. */
int r2s_posix_open(const char *path);

#ifdef __cplusplus
}
#endif

#endif
