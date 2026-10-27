/* topics: the topic table the core reads through count, get and copy. The
 * static paths bind a constant table in flash; the runtime path binds its
 * RAM table here. */
#ifndef R2S_TOPICS_H
#define R2S_TOPICS_H

#include <stdint.h>

#include "r2s/r2s.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Topics the core can hold per-topic state for. The generated configuration
 * raises it when the firmware needs more. */
#ifndef R2S_TOPIC_MAX_COUNT
#define R2S_TOPIC_MAX_COUNT 16u
#endif

/* Table placement lives in r2s_arch.h, so the generator emits the same macro
 * the core reads. */
#include "r2s/r2s_arch.h"

/* One announced topic: its user topic_id, its CTL-4 flags, the first 8 bytes
 * of its RIHS01 type hash, and its type and topic names. */
typedef struct {
    uint8_t topic_id;
    uint8_t flags;
    uint8_t type_hash[8];
    const char *type_name;
    const char *topic_name;
} r2s_topic_t;

/* Binds the table the accessors read. The table outlives the call: the static
 * paths pass their constant table in flash. The bind is the single gate for
 * the table: it refuses a missing table, a count above R2S_TOPIC_MAX_COUNT,
 * a missing name, a name beyond the varint range, or an entry that does not
 * fit the announce frame, leaving the previous binding unchanged. The table
 * itself carries R2S_FLASH while its name strings stay in RAM, so every read
 * below goes through R2S_FLASH_READ_BYTE. */
r2s_status_t r2s_topics_configure(const r2s_topic_t *topics, uint8_t count);

/* The number of bound topics, 0 when no table is bound. */
uint8_t r2s_topic_count(void);

/* The entry at index, or NULL past the count. The pointer stays valid until
 * the next configure or test reset. The entry itself may live in flash: only
 * dereference the pointer for a RAM table, and copy a flash entry out first. */
const r2s_topic_t *r2s_topic_get(uint8_t index);

/* A RAM copy of the entry at index, read through R2S_FLASH_READ_BYTE, so the
 * announce path serves a flash table and a RAM table alike. The names stay
 * RAM strings through the copy. R2S_OK, or R2S_ERR_BOUNDS past the count or
 * for a NULL out, leaving out unchanged. */
r2s_status_t r2s_topic_copy(uint8_t index, r2s_topic_t *out);

/* The index of the entry with this topic_id, or 0xFF when the table names no
 * such topic. */
uint8_t r2s_topic_find(uint8_t topic_id);

#ifdef R2S_TESTING
/* Host test builds only: unbinds the table, as r2s_test_reset() calls it. */
void r2s_topics_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
