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

/// Topics the core can hold per-topic state for.
/**
 * The generated configuration raises it when the firmware needs more.
 */
#ifndef R2S_TOPIC_MAX_COUNT
#define R2S_TOPIC_MAX_COUNT 16u
#endif

/* Table placement lives in r2s_arch.h, so the generator emits the same macro
 * the core reads. */
#include "r2s/r2s_arch.h"

/// One announced topic.
/**
 * Its user topic_id, its CTL-4 flags, the first 8 bytes of its RIHS01 type
 * hash, and its type and topic names.
 */
typedef struct
{
  uint8_t topic_id;      ///< The user topic ID.
  uint8_t flags;         ///< The CTL-4 flags.
  uint8_t type_hash[8];  ///< The first 8 bytes of the RIHS01 type hash.
  const char *type_name;  ///< The type name.
  const char *topic_name;  ///< The topic name.
} r2s_topic_t;

/// Binds the table the accessors read.
/**
 * The table outlives the call: the static paths pass their constant table in
 * flash. The bind is the single gate: it refuses a missing table, a count
 * above R2S_TOPIC_MAX_COUNT, a missing name, a name beyond the varint range,
 * or an entry that does not fit the announce frame, leaving the previous
 * binding unchanged. A RAM table binds through r2s_topics_configure_ram
 * instead, since the readers cannot tell flash from RAM on AVR.
 *
 * \param[in] topics the table to bind.
 * \param[in] count how many entries topics holds.
 * \return R2S_OK, or R2S_ERR_BOUNDS on a refused table.
 * \pre The caller keeps topics alive past the call.
 */
r2s_status_t r2s_topics_configure(const r2s_topic_t *topics, uint8_t count);

/// Binds a RAM table, as the runtime registration path fills one.
/**
 * The same gate as r2s_topics_configure(), with plain reads: the right entry
 * point for a table the firmware wrote at runtime rather than carrying in
 * flash.
 *
 * \param[in] topics the table to bind.
 * \param[in] count how many entries topics holds.
 * \return R2S_OK, or R2S_ERR_BOUNDS on a refused table.
 * \pre The caller keeps topics alive past the call.
 */
r2s_status_t r2s_topics_configure_ram(const r2s_topic_t *topics, uint8_t count);

/// The number of bound topics, 0 when no table is bound.
/**
 * \return The bound count.
 */
uint8_t r2s_topic_count(void);

/// The entry at index, or NULL past the count.
/**
 * The pointer stays valid until the next configure or test reset. Only
 * dereference it for a RAM table: a flash entry's bytes live in the flash
 * address space on AVR, where a plain dereference fetches the wrong bytes.
 * Copy a flash entry out first.
 *
 * \param[in] index which entry to read.
 * \return The entry, or NULL past the count.
 */
const r2s_topic_t * r2s_topic_get(uint8_t index);

/// A RAM copy of the entry at index.
/**
 * Read through the bound table's own readers, so the announce path serves a
 * flash table and a RAM table alike. The names stay RAM strings through the
 * copy.
 *
 * \param[in] index which entry to copy.
 * \param[out] out where to write the copy.
 * \return R2S_OK, or R2S_ERR_BOUNDS past the count or for a NULL out,
 * leaving out unchanged.
 */
r2s_status_t r2s_topic_copy(uint8_t index, r2s_topic_t *out);

/// The index of the entry with this topic_id, or 0xFF when missing.
/**
 * \param[in] topic_id the user topic ID to look up.
 * \return The entry index, or 0xFF when the table names no such topic.
 */
uint8_t r2s_topic_find(uint8_t topic_id);

#ifdef R2S_TESTING
/// Unbinds the table for host test builds.
/**
 * The same reset, as r2s_test_reset() calls it.
 */
void r2s_topics_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
