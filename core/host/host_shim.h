/* libr2s_host shim: the failure hook and error cell the Python driver uses. */
#ifndef R2S_HOST_SHIM_H
#define R2S_HOST_SHIM_H

/* The last fake port misuse, or an empty string when none happened. The
 * driver asserts it stays empty after every step. */
const char *r2s_host_error(void);

#endif
