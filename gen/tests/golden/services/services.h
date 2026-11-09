/* Services in configuration order: IDs run from 0, each service
 * names its request and response by the descriptor indices the
 * depth-first walk fixed (DSC-11), and the role byte is 0 when the
 * MCU serves the service and 1 when it calls it (SVR-2). */

#define R2S_SERVICE_COUNT 2u
#ifndef R2S_SERVICE_MAX_COUNT
#define R2S_SERVICE_MAX_COUNT 2u
#endif

#include "r2s/r2s_services.h"

#define R2S_SERVICE_RESET_ID 0u
#define R2S_SERVICE_TRIGGER_ID 1u
#define R2S_SERVICE_RESET_ROLE 0x00u
#define R2S_SERVICE_TRIGGER_ROLE 0x01u
#define R2S_SERVICE_RESET_REQUEST_TYPE 0u
#define R2S_SERVICE_TRIGGER_REQUEST_TYPE 0u
#define R2S_SERVICE_RESET_RESPONSE_TYPE 1u
#define R2S_SERVICE_TRIGGER_RESPONSE_TYPE 1u

/* The announced services and the service verdicts (SVR-7); a board
 * that renames services at launch copies this table. */
extern const r2s_service_t R2S_FLASH r2s_services[R2S_SERVICE_COUNT];
