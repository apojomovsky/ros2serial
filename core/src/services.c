/* services: the MCU side of the services channel. The table below is the
 * firmware's announced set; the session calls in for channel 7 frames. */
#include "r2s/r2s_services.h"

#include <stddef.h>

#ifdef R2S_WITH_SERVICES
r2s_status_t r2s_services_configure(const r2s_service_t *services, uint8_t count)
{
    (void)services;
    (void)count;
    return R2S_ERR_BOUNDS;
}

uint8_t r2s_services_count(void)
{
    return 0u;
}

const r2s_service_t *r2s_services_get(uint8_t index)
{
    (void)index;
    return NULL;
}

uint8_t r2s_services_status(uint8_t index)
{
    (void)index;
    return 0xFFu;
}

uint16_t r2s_services_declare_len(uint8_t index)
{
    (void)index;
    return 0u;
}

r2s_status_t r2s_service_call(uint8_t index, const uint8_t *payload, uint16_t len, uint8_t *req_id)
{
    (void)index;
    (void)payload;
    (void)len;
    (void)req_id;
    return R2S_ERR_BOUNDS;
}

r2s_status_t r2s_service_take_request(uint8_t *index, uint8_t *req_id, uint8_t *payload, uint16_t cap,
                                      uint16_t *len)
{
    (void)index;
    (void)req_id;
    (void)payload;
    (void)cap;
    (void)len;
    return R2S_SVC_EMPTY;
}

r2s_status_t r2s_service_send_response(uint8_t req_id, uint8_t status, const uint8_t *payload, uint16_t len)
{
    (void)req_id;
    (void)status;
    (void)payload;
    (void)len;
    return R2S_ERR_BOUNDS;
}

r2s_status_t r2s_service_take_response(uint8_t *req_id, uint8_t *status, uint8_t *payload, uint16_t cap,
                                       uint16_t *len)
{
    (void)req_id;
    (void)status;
    (void)payload;
    (void)cap;
    (void)len;
    return R2S_SVC_EMPTY;
}
#endif

#ifdef R2S_TESTING
void r2s_services_test_reset(void)
{
#ifdef R2S_WITH_SERVICES
#endif
}
#endif

#if !defined(R2S_WITH_SERVICES) && !defined(R2S_TESTING)
/* ISO C needs at least one declaration in a translation unit. */
typedef int r2s_services_unused;
#endif
