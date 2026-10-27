/* topics: the topic table the core reads through count and get. */
#include <stddef.h>
#include <string.h>

#include "r2s/r2s_arch.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_topics.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"

static struct {
    const r2s_topic_t *table;
    uint8_t count;
    uint8_t table_in_flash;
} bound;

/* One entry in RAM. A flash table goes byte by byte through
 * R2S_FLASH_READ_BYTE; a RAM table is a plain copy. The name strings stay in
 * RAM either way, so the copied pointers read as plain memory. */
static void table_copy(const r2s_topic_t *table, uint8_t index, r2s_topic_t *out, uint8_t from_flash)
{
    if (from_flash != 0u) {
        const uint8_t *src = (const uint8_t *)&table[index];
        uint8_t *dst = (uint8_t *)out;
        size_t i;

        for (i = 0u; i < sizeof(r2s_topic_t); i++) {
            dst[i] = R2S_FLASH_READ_BYTE(src + i);
        }
    } else {
        *out = table[index];
    }
}

/* One entry's topic ID, from flash or RAM like the copy above. The dispatch
 * path compares IDs on every frame, where copying whole entries would waste
 * the stack. */
static uint8_t table_id(const r2s_topic_t *table, uint8_t index, uint8_t from_flash)
{
    if (from_flash != 0u) {
        return R2S_FLASH_READ_BYTE(&table[index].topic_id);
    }
    return table[index].topic_id;
}

static r2s_status_t bind(const r2s_topic_t *topics, uint8_t count, uint8_t from_flash)
{
    uint8_t i;
    uint8_t len_buf[R2S_VARINT_MAX_BYTES];

    if ((topics == NULL && count != 0u) || count > R2S_TOPIC_MAX_COUNT) {
        return R2S_ERR_BOUNDS;
    }
    for (i = 0u; i < count; i++) {
        r2s_topic_t entry;
        size_t type_len;
        size_t topic_len;
        uint32_t need;

        table_copy(topics, i, &entry, from_flash);
        if (entry.type_name == NULL || entry.topic_name == NULL) {
            return R2S_ERR_BOUNDS;
        }
        type_len = strlen(entry.type_name);
        topic_len = strlen(entry.topic_name);
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
    bound.table_in_flash = from_flash;
    return R2S_OK;
}

r2s_status_t r2s_topics_configure(const r2s_topic_t *topics, uint8_t count)
{
    return bind(topics, count, 1u);
}

r2s_status_t r2s_topics_configure_ram(const r2s_topic_t *topics, uint8_t count)
{
    return bind(topics, count, 0u);
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
        if (table_id(bound.table, i, bound.table_in_flash) == topic_id) {
            return i;
        }
    }
    return 0xFFu;
}

r2s_status_t r2s_topic_copy(uint8_t index, r2s_topic_t *out)
{
    if (out == NULL || index >= bound.count) {
        return R2S_ERR_BOUNDS;
    }
    table_copy(bound.table, index, out, bound.table_in_flash);
    return R2S_OK;
}

#ifdef R2S_TESTING
void r2s_topics_test_reset(void)
{
    bound.table = NULL;
    bound.count = 0u;
    bound.table_in_flash = 0u;
}
#endif
