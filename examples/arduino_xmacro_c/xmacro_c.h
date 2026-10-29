/* Shared declarations between the C sketch file and the Arduino shim. */

#ifndef XMACRO_C_H
#define XMACRO_C_H

#ifdef __cplusplus
extern "C" {
#endif

void xmacro_setup(void);
void xmacro_loop(void);

#ifdef __cplusplus
}
#endif

#endif
