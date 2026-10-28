/* runtime: the ROS 2-named runtime API. Topics are registered while the
 * firmware runs, into the core's RAM topic table, so a sketch needs no
 * generated topic list. The core reaches the subscribers through one
 * dispatch hook this layer implements, and the core itself keeps no
 * function pointer. */
#ifndef R2S_RUNTIME_HPP
#define R2S_RUNTIME_HPP

#include <float.h>
#include <stdint.h>

#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_topics.h"

#ifdef ARDUINO
#include "r2s_arduino_port.h"
#endif
/* A build whose double is 32 bits travels narrow on every topic, so the
 * announced flags carry it (ENC-12, CTL-4). */
#if DBL_MANT_DIG == FLT_MANT_DIG
#define R2S_RUNTIME_NARROW_FLAG 0x02u
#else
#define R2S_RUNTIME_NARROW_FLAG 0x00u
#endif

namespace r2s
{
/* What every message type must provide: the descriptor table that encodes
 * and decodes it, its index in that table, its type name and the first
 * eight bytes of its type hash. The generated message headers specialize
 * this template. */
template <typename T> struct msg_traits;

namespace detail
{
/* One subscriber's storage and callback, typed by the message it carries. */
template <typename T> struct subscriber {
    const r2s_codec_table_t *table;
    uint16_t type_index;
    T *storage;
    void (*callback)(const T &);
};

/* Decodes a payload into the subscriber's storage and calls its callback.
 * The registry stores this per subscriber, so the dispatch stays typed
 * while the core sees only a plain function. */
template <typename T>
void dispatch_subscriber(void *record, const uint8_t *payload, uint16_t len, uint8_t narrow)
{
    subscriber<T> *sub = static_cast<subscriber<T> *>(record);
    uint8_t field = 0xFFu;

    if (r2s_codec_decode(sub->table, sub->type_index, payload, len, narrow, sub->storage, &field) != R2S_OK) {
        r2s_framer_count_refused();
        return;
    }
    sub->callback(*sub->storage);
}

/* The registry's seam. The registry itself lives in runtime.cpp, so the
 * core stays free of C++ and of the callbacks this layer stores. */
r2s_status_t register_publisher(const char *topic, const r2s_codec_table_t *table, uint16_t type_index,
                                const char *type_name, const uint8_t *type_hash, uint8_t *topic_id);
/* A subscriber's table and type index live in its own typed record, so the
 * registry needs only the record and the dispatch that reaches it. */
r2s_status_t register_subscriber(const char *topic, const char *type_name, const uint8_t *type_hash,
                                 void *record, void (*dispatch)(void *, const uint8_t *, uint16_t, uint8_t),
                                 uint8_t *topic_id);
r2s_status_t publish(uint8_t topic_id, const void *msg);
void spin(void);
bool registration_error(void);
} // namespace detail

/* The firmware's node: it names the session and owns the topics declared
 * against it. One node per firmware, since the core has one session. */
class Node
{
  public:
    /* Names the node the session announces. A second node sets the
     * registration error, because the core has one session to announce. */
    explicit Node(const char *name);

    /* Runs the session: the first call binds the topic table and starts
     * the handshake, the later ones serve the link. */
    void spin_some();

    /* True while the handshake completed and user traffic may flow. */
    bool connected() const;

    /* Milliseconds since boot, taken from the port tick. The host's time
     * sync replaces it with the shared clock when that channel lands. */
    uint32_t now() const;

    /* True when the firmware asked for something the runtime refused: a
     * second node, a topic declared after the table closed, or a table
     * the session would not bind. */
    bool registration_error() const;

    /* Starts the port at baud. On a board the port glue also takes the
     * stream, so the core reads it; elsewhere the port is started and
     * nothing else, so a host build needs no Arduino header. The port
     * type must offer begin(unsigned long), as every Arduino stream does. */
    template <typename PortT> void begin(PortT &port, unsigned long baud)
    {
#ifdef ARDUINO
        detail::arduino_port_begin(port, baud);
#else
        port.begin(baud);
#endif
    }

    /* The registration the Publisher and the Subscription call. They are
     * public because the templates reach them, and a topic they refuse
     * leaves the node with its registration error set. */
    r2s_status_t register_publisher(const char *topic, const r2s_codec_table_t *table, uint16_t type_index,
                                    const char *type_name, const uint8_t *type_hash, uint8_t *topic_id);
    r2s_status_t register_subscriber(const char *topic, const char *type_name, const uint8_t *type_hash,
                                     void *record,
                                     void (*dispatch)(void *, const uint8_t *, uint16_t, uint8_t),
                                     uint8_t *topic_id);

  private:
    Node(const Node &);
    Node &operator=(const Node &);

    const char *m_name;
    bool m_owned;
};

/* Publishes messages of one type on one topic. */
template <typename T> class Publisher
{
  public:
    /* Announces the topic against the node. A node whose table already
     * closed refuses it, and the node keeps its registration error. The
     * parameter is not named node, since a sketch names its own node
     * that way and the warning would fire on every such sketch. */
    Publisher(Node &owner, const char *topic)
    {
        uint8_t id = 0xFFu;

        m_topic_id = 0xFFu;
        (void)owner.register_publisher(topic, msg_traits<T>::table(), msg_traits<T>::type_index(),
                                       msg_traits<T>::type_name(), msg_traits<T>::type_hash(), &id);
        m_topic_id = id;
    }

    /* Sends the message on the wire. Fails with the not-connected error
     * while the session announces, with the rejected error when the host
     * did not accept the topic, and with the bounds error when the
     * message does not fit; each failure sends nothing. */
    r2s_status_t publish(const T &msg)
    {
        if (m_topic_id == 0xFFu) {
            return R2S_ERR_TOPIC_REJECTED;
        }
        return detail::publish(m_topic_id, &msg);
    }

  private:
    uint8_t m_topic_id;
};

/* Receives messages of one type on one topic, and calls the handler with
 * the message it decoded. Keep it alive while the node spins: the registry
 * keeps the address of its record, which carries the storage and the
 * handler, so destroying it first leaves a dangling record. Sketches
 * declare it at file scope, next to the node. */
template <typename T> class Subscription
{
  public:
    /* Announces the topic against the node and stores the handler. A
     * capture-less lambda converts to the function pointer the handler
     * takes. The parameter is not named node, since a sketch names its
     * own node that way and the warning would fire on every such
     * sketch. */
    Subscription(Node &owner, const char *topic, void (*callback)(const T &msg))
    {
        uint8_t id = 0xFFu;

        m_topic_id = 0xFFu;
        m_subscriber.table = msg_traits<T>::table();
        m_subscriber.type_index = msg_traits<T>::type_index();
        m_subscriber.storage = &m_storage;
        m_subscriber.callback = callback;
        (void)owner.register_subscriber(topic, msg_traits<T>::type_name(), msg_traits<T>::type_hash(),
                                        &m_subscriber, &detail::dispatch_subscriber<T>, &id);
        m_topic_id = id;
    }

  private:
    /* Each subscription holds its own copy of the message, as the
     * rosserial API does, so two subscriptions never share a buffer. */
    T m_storage;
    detail::subscriber<T> m_subscriber;
    uint8_t m_topic_id;
};
} // namespace r2s

#ifdef R2S_TESTING
/* Host test builds only: empties the registry, so one binary runs many
 * tests from the same power-up state. */
extern "C" void r2s_runtime_test_reset(void);
#endif

#endif
