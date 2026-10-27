/* topics: the topic table the core reads through count and get. */
#include <stddef.h>
#include <string.h>

#include "r2s/r2s_framer.h"
#include "r2s/r2s_topics.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"

static struct {
    const r2s_topic_t *table;
    uint8_t count;
} bound;

r2s_status_t r2s_topics_configure(const r2s_topic_t *topics, uint8_t count)
{
    uint8_t i;
    uint8_t len_buf[R2S_VARINT_MAX_BYTES];

    if ((topics == NULL && count != 0u) || count > R2S_TOPIC_MAX_COUNT) {
        return R2S_ERR_BOUNDS;
    }
    for (i = 0u; i < count; i++) {
        size_t type_len;
        size_t topic_len;
        uint32_t need;

        if (topics[i].type_name == NULL || topics[i].topic_name == NULL) {
            return R2S_ERR_BOUNDS;
        }
        type_len = strlen(topics[i].type_name);
        topic_len = strlen(topics[i].topic_name);
        if (type_len > R2S_VARINT_MAX || topic_len > R2S_VARINT_MAX) {
            return R2S_ERR_BOUNDS;
        }
        /* 12 fixed bytes, the length varints and the names: exact. The sums
         * run in 32 bits, because on a 16-bit size_t a name near 64 KB would
         * wrap a size_t sum back under the limit. */
        need = (uint32_t)12u + (uint32_t)r2s_varint_encode((uint16_t)type_len, len_buf) + (uint32_t)type_len +
               (uint32_t)r2s_varint_encode((uint16_t)topic_len, len_buf) + (uint32_t)topic_len;
        if (need > (uint32_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD)) {
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
