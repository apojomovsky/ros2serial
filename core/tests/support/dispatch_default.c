/* Default user-frame dispatch for test binaries without a topic
 * configuration: accepted and ignored, as a build without the codec behaves.
 * The link tests override it with the strong symbol from the handwritten
 * firmware wiring. Test-only: the core never sees this file. */
#include "handwritten.h"

__attribute__((weak)) void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                                              uint16_t len)
{
    (void)topic_id;
    (void)seq;
    (void)payload;
    (void)len;
}
