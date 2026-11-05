/* params: the MCU side of the parameters channel. The table below is the
 * firmware's live values; the session calls in for channel 3 frames. */
#include "r2s/r2s_params.h"

#include <string.h>

#include "r2s/r2s.h"

#ifdef R2S_WITH_PARAMS
r2s_status_t r2s_params_configure(const r2s_param_t *params, uint8_t count)
{
    (void)params;
    (void)count;
    return R2S_ERR_BOUNDS;
}

uint8_t r2s_params_count(void)
{
    return 0u;
}

const r2s_param_t *r2s_params_get(uint8_t index)
{
    (void)index;
    return NULL;
}
#endif

#ifdef R2S_TESTING
void r2s_params_test_reset(void)
{
}
#endif
