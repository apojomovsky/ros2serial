/* topics: the topic table the core reads through count and get. The static paths
 * bind a constant table in flash; the runtime path binds its RAM table here. */
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

/* Flash placement for the topic table. The canonical home of this macro is
 * r2s_arch.h (C12); until it lands, the topics header carries the same
 * guarded default as the codec, so generated tables emit the same bytes. */
#ifndef R2S_FLASH
#ifdef __AVR__
#include <avr/pgmspace.h>
#define R2S_FLASH PROGMEM
#else
#define R2S_FLASH
#endif
#endif

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
 * fit the announce frame, leaving the previous binding unchanged. Reading a
 * table in flash needs the C12 byte-wise readers; until they land, direct
 * reads suit RAM and host builds. */
r2s_status_t r2s_topics_configure(const r2s_topic_t *topics, uint8_t count);

/* The number of bound topics, 0 when no table is bound. */
uint8_t r2s_topic_count(void);

/* The entry at index, or NULL past the count. The pointer stays valid until
 * the next configure or test reset. */
const r2s_topic_t *r2s_topic_get(uint8_t index);

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
