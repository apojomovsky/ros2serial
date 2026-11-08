/* The virtual MCU's parameter table: a bool led and an i64 rate, announced
 * so the end-to-end suite sets them over the reliable channel. */
#ifndef R2S_POSIX_PARAMS_H
#define R2S_POSIX_PARAMS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hands the core the table below. Returns 0, or -1 when the table is unfit,
 * so the firmware refuses to announce a lie. */
int r2s_posix_params_init(void);

/* Prints each applied value the last report has not covered yet, so the
 * end-to-end suite reads the MCU's live values off stderr. */
void r2s_posix_params_report(void);

#ifdef __cplusplus
}
#endif

#endif
