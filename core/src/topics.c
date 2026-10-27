/* topics: the topic table the core reads through count and get. */
#include <stddef.h>

#include "r2s/r2s_topics.h"

static struct {
    const r2s_topic_t *table;
    uint8_t count;
} bound;

r2s_status_t r2s_topics_configure(const r2s_topic_t *topics, uint8_t count)
{
    uint8_t i;

    if ((topics == NULL && count != 0u) || count > R2S_TOPIC_MAX_COUNT) {
        return R2S_ERR_BOUNDS;
    }
    for (i = 0u; i < count; i++) {
        if (topics[i].type_name == NULL || topics[i].topic_name == NULL) {
            return R2S_ERR_BOUNDS;
        }
    }
    bound.table = topics;
    bound.count = count;
    return R2S_OK;
}

uint8_t r2s_topic_count(void)
{
    return bound.count;
}

const r2s_topic_t *r2s_topic_get(uint8_t index)
{
    if (index >= bound.count) {
        return NULL;
    }
    return &bound.table[index];
}

uint8_t r2s_topic_find(uint8_t topic_id)
{
    uint8_t i;

    for (i = 0u; i < bound.count; i++) {
        if (bound.table[i].topic_id == topic_id) {
            return i;
        }
    }
    return 0xFFu;
}

#ifdef R2S_TESTING
void r2s_topics_test_reset(void)
{
    bound.table = NULL;
    bound.count = 0u;
}
#endif
