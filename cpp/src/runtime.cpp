/* runtime: the registry behind the ROS 2-named runtime API. It holds the
 * RAM topic table the core announces, the records the publish and dispatch
 * paths use, and the one dispatch hook the core calls. The core itself
 * keeps no function pointer; the callbacks live here. */
#include "r2s/runtime.hpp"

#include "r2s/r2s_tx.h"

/* The RAM the runtime costs, named so a size report can find it: the topic
 * table the core announces, and the records beside it. */
extern "C" {
r2s_topic_t r2s_runtime_topics[R2S_TOPIC_MAX_COUNT];
}

/* One publisher's codec parameters, kept beside the entry it announces. */
struct publisher_record {
    const r2s_codec_table_t *table;
    uint16_t type_index;
    uint8_t topic_id;
};

/* One subscriber's storage and the typed dispatch that reaches it. */
struct subscriber_record {
    void *record;
    void (*dispatch)(void *, const uint8_t *, uint16_t, uint8_t);
    uint8_t topic_id;
};

extern "C" {
publisher_record r2s_runtime_publishers[R2S_TOPIC_MAX_COUNT];
subscriber_record r2s_runtime_subscribers[R2S_TOPIC_MAX_COUNT];
}

namespace
{
/* The whole runtime state beside the tables: the counts, the flags the
 * firmware asks about, and the node that owns the session. */
struct registry {
    uint8_t topic_count;
    uint8_t publisher_count;
    uint8_t subscriber_count;
    uint8_t closed;
    uint8_t error;
    uint8_t node_claimed;
    uint8_t bound;
    const char *node_name;
};

registry reg;

/* The first user topic id; the ids below 16 are the system channels
 * (FRM-3). */
#define R2S_RUNTIME_FIRST_TOPIC_ID 16u

uint8_t find_publisher(uint8_t topic_id)
{
    uint8_t i;

    for (i = 0u; i < reg.publisher_count; i++) {
        if (r2s_runtime_publishers[i].topic_id == topic_id) {
            return i;
        }
    }
    return 0xFFu;
}

uint8_t find_subscriber(uint8_t topic_id)
{
    uint8_t i;

    for (i = 0u; i < reg.subscriber_count; i++) {
        if (r2s_runtime_subscribers[i].topic_id == topic_id) {
            return i;
        }
    }
    return 0xFFu;
}

/* Adds one entry to the table and hands back the id it took. The ids run
 * from 16 in construction order, and the flags carry the direction and the
 * narrow bit of the build (CTL-4). */
r2s_status_t add_topic(const char *topic, const char *type_name, const uint8_t *type_hash, uint8_t direction,
                       uint8_t *topic_id)
{
    r2s_topic_t *entry;
    uint8_t i;

    if (reg.closed != 0u) {
        reg.error = 1u;
        return R2S_ERR_BOUNDS;
    }
    if (reg.topic_count >= R2S_TOPIC_MAX_COUNT) {
        reg.error = 1u;
        return R2S_ERR_BOUNDS;
    }
    if (topic == nullptr || type_name == nullptr || type_hash == nullptr) {
        reg.error = 1u;
        return R2S_ERR_BOUNDS;
    }
    entry = &r2s_runtime_topics[reg.topic_count];
    entry->topic_id = (uint8_t)(R2S_RUNTIME_FIRST_TOPIC_ID + reg.topic_count);
    entry->flags = (uint8_t)(direction | R2S_RUNTIME_NARROW_FLAG);
    for (i = 0u; i < 8u; i++) {
        entry->type_hash[i] = type_hash[i];
    }
    entry->type_name = type_name;
    entry->topic_name = topic;
    *topic_id = entry->topic_id;
    reg.topic_count++;
    return R2S_OK;
}
} // namespace

namespace r2s
{
namespace detail
{
r2s_status_t register_publisher(const char *topic, const r2s_codec_table_t *table, uint16_t type_index,
                                const char *type_name, const uint8_t *type_hash, uint8_t *topic_id)
{
    uint8_t id = 0xFFu;
    r2s_status_t rc = add_topic(topic, type_name, type_hash, 0x00u, &id);

    if (rc != R2S_OK) {
        return rc;
    }
    r2s_runtime_publishers[reg.publisher_count].table = table;
    r2s_runtime_publishers[reg.publisher_count].type_index = type_index;
    r2s_runtime_publishers[reg.publisher_count].topic_id = id;
    reg.publisher_count++;
    *topic_id = id;
    return R2S_OK;
}

r2s_status_t register_subscriber(const char *topic, const char *type_name, const uint8_t *type_hash,
                                 void *record, void (*dispatch)(void *, const uint8_t *, uint16_t, uint8_t),
                                 uint8_t *topic_id)
{
    uint8_t id = 0xFFu;
    r2s_status_t rc = add_topic(topic, type_name, type_hash, 0x01u, &id);

    if (rc != R2S_OK) {
        return rc;
    }
    r2s_runtime_subscribers[reg.subscriber_count].record = record;
    r2s_runtime_subscribers[reg.subscriber_count].dispatch = dispatch;
    r2s_runtime_subscribers[reg.subscriber_count].topic_id = id;
    reg.subscriber_count++;
    *topic_id = id;
    return R2S_OK;
}

r2s_status_t publish(uint8_t topic_id, const void *msg)
{
    uint8_t index = find_publisher(topic_id);
    uint8_t table_index;
    uint8_t narrow;
    uint8_t seq;
    uint8_t field = 0xFFu;
    r2s_codec_out_t out = {nullptr, 0u, 0u, 0u};
    int8_t rc;

    if (index == 0xFFu) {
        return R2S_ERR_TOPIC_REJECTED;
    }
    if (r2s_connected() == 0u) {
        return R2S_ERR_NOT_CONNECTED;
    }
    table_index = r2s_session_find_topic(topic_id);
    if (table_index == 0xFFu || r2s_session_topic_status(table_index) != 0u) {
        return R2S_ERR_TOPIC_REJECTED;
    }
    /* The announced flags are the truth for the wire, so the narrow bit
     * comes from them and not from the build (CTL-5). */
    narrow = (uint8_t)(r2s_session_topic_flags(table_index) & 0x02u);
    /* The payload is measured first, so a message that does not fit fails
     * before anything is written (DSC-35). */
    rc = r2s_codec_encode(r2s_runtime_publishers[index].table, r2s_runtime_publishers[index].type_index, msg,
                          narrow, &out, &field);
    if (rc != R2S_OK) {
        return rc;
    }
    seq = r2s_session_tx_seq(table_index);
    rc = r2s_tx_begin(topic_id, seq, out.len);
    if (rc != R2S_OK) {
        return rc;
    }
    r2s_session_advance_tx_seq(table_index);
    out.to_tx = 1u;
    out.len = 0u;
    (void)r2s_codec_encode(r2s_runtime_publishers[index].table, r2s_runtime_publishers[index].type_index, msg,
                           narrow, &out, &field);
    (void)r2s_tx_end();
    return R2S_OK;
}

void spin(void)
{
    r2s_spin_some();
}

bool registration_error(void)
{
    return reg.error != 0u;
}
} // namespace detail

Node::Node(const char *name) : m_name(name), m_owned(false)
{
    /* The core has one session, so one node announces it. A second node
     * sets the error flag instead of taking the session away. */
    if (reg.node_claimed != 0u) {
        reg.error = 1u;
        return;
    }
    reg.node_claimed = 1u;
    reg.node_name = name;
    m_owned = true;
}

void Node::spin_some()
{
    if (m_owned && reg.bound == 0u) {
        /* The table closes with the first spin: the session binds it and
         * starts announcing, and a topic declared after that is refused. */
        reg.closed = 1u;
        reg.bound = 1u;
        if (reg.node_name == nullptr ||
            r2s_session_configure_ram(reg.node_name, r2s_runtime_topics, reg.topic_count) != R2S_OK) {
            reg.error = 1u;
            return;
        }
        r2s_init();
    }
    detail::spin();
}

bool Node::connected() const
{
    return r2s_connected() != 0u;
}

uint32_t Node::now() const
{
    /* The tick is not always a millisecond: the session converts it with
     * the same factor, so the two agree on the time of day (CTL-8). The
     * tick is divided first, since a product of the tick and the scale
     * would overflow 32 bits on a small target. */
    uint32_t tick = r2s_port_tick();

    if (R2S_TICK_HZ == 1000u) {
        return tick;
    }
    return (tick / (uint32_t)R2S_TICK_HZ) * 1000u +
           (((tick % (uint32_t)R2S_TICK_HZ) * 1000u) / (uint32_t)R2S_TICK_HZ);
}

bool Node::registration_error() const
{
    return detail::registration_error();
}

r2s_status_t Node::register_publisher(const char *topic, const r2s_codec_table_t *table, uint16_t type_index,
                                      const char *type_name, const uint8_t *type_hash, uint8_t *topic_id)
{
    if (!m_owned) {
        reg.error = 1u;
        return R2S_ERR_BOUNDS;
    }
    return detail::register_publisher(topic, table, type_index, type_name, type_hash, topic_id);
}

r2s_status_t Node::register_subscriber(const char *topic, const char *type_name, const uint8_t *type_hash,
                                       void *record,
                                       void (*dispatch)(void *, const uint8_t *, uint16_t, uint8_t),
                                       uint8_t *topic_id)
{
    if (!m_owned) {
        reg.error = 1u;
        return R2S_ERR_BOUNDS;
    }
    return detail::register_subscriber(topic, type_name, type_hash, record, dispatch, topic_id);
}
} // namespace r2s

/* The hook the core calls for a user frame. It is weak, so a sketch that
 * links the static API keeps its own generated dispatch. */
extern "C" __attribute__((weak)) void r2s_codec_dispatch(uint8_t topic_id, uint8_t seq,
                                                         const uint8_t *payload, uint16_t len)
{
    uint8_t index = find_subscriber(topic_id);
    uint8_t table_index;
    uint8_t narrow;

    if (index == 0xFFu) {
        r2s_framer_count_refused();
        return;
    }
    table_index = r2s_session_find_topic(topic_id);
    if (table_index == 0xFFu || r2s_session_topic_status(table_index) != 0u) {
        r2s_framer_count_refused();
        return;
    }
    narrow = (uint8_t)(r2s_session_topic_flags(table_index) & 0x02u);
    (void)r2s_session_note_rx_seq(table_index, seq);
    r2s_runtime_subscribers[index].dispatch(r2s_runtime_subscribers[index].record, payload, len, narrow);
}

#ifdef R2S_TESTING
extern "C" void r2s_runtime_test_reset(void)
{
    uint8_t i;

    for (i = 0u; i < R2S_TOPIC_MAX_COUNT; i++) {
        r2s_runtime_topics[i].topic_id = 0u;
        r2s_runtime_topics[i].flags = 0u;
        r2s_runtime_topics[i].type_hash[0] = 0u;
        r2s_runtime_topics[i].type_name = nullptr;
        r2s_runtime_topics[i].topic_name = nullptr;
        r2s_runtime_publishers[i].table = nullptr;
        r2s_runtime_publishers[i].type_index = 0u;
        r2s_runtime_publishers[i].topic_id = 0u;
        r2s_runtime_subscribers[i].record = nullptr;
        r2s_runtime_subscribers[i].dispatch = nullptr;
        r2s_runtime_subscribers[i].topic_id = 0u;
    }
    reg.topic_count = 0u;
    reg.publisher_count = 0u;
    reg.subscriber_count = 0u;
    reg.closed = 0u;
    reg.error = 0u;
    reg.node_claimed = 0u;
    reg.bound = 0u;
    reg.node_name = nullptr;
}
#endif
