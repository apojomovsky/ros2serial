/* session: the MCU side of the handshake and the link. It announces the
 * firmware on boot, answers the host through ANNOUNCING, and sends heartbeats
 * while CONNECTED (spec/protocol.md, sections 7 and 8). */
#include "r2s/r2s_session.h"

#include <float.h>
#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"

#ifdef R2S_WITH_CODEC
#include "r2s/r2s_codec.h"
#endif

/* The link states (SES-1, SES-6, SES-7). */
#define ANNOUNCING 0u
#define CONNECTED 1u

/* Control opcodes (CTL-16). */
#define OP_HELLO_REQ 0x01u
#define OP_HELLO 0x02u
#define OP_TOPIC_REQ 0x03u
#define OP_TOPIC 0x04u
#define OP_TOPIC_STATUS 0x05u
#define OP_READY 0x06u

/* ANNOUNCING repeats HELLO this often without host traffic (SES-5), and
 * answers other host frames with HELLO at most this often (SES-3). */
#define HELLO_REPEAT_MS 1000u
#define HELLO_NACK_MS 100u

/* The CTL-2 feature bits this build sets. Time, log, parameters and the
 * reliable channel set theirs when they land; bit 4 is informational. */
#if DBL_MANT_DIG == FLT_MANT_DIG
#define R2S_FEATURE_BITS 0x10u
#else
#define R2S_FEATURE_BITS 0x00u
#endif

/* tick_hz is 1000 times a divisor of 10^6, so a tick converts to nanoseconds
 * with integer math alone (CTL-8). A failing build names the rule here. */
typedef char r2s_tick_hz_form[(R2S_TICK_HZ >= 1000u && R2S_TICK_HZ % 1000u == 0u &&
                               (1000000u % (R2S_TICK_HZ / 1000u)) == 0u)
                                  ? 1
                                  : -1];

/* The bound table holds at most R2S_TOPIC_MAX_COUNT entries, and the session
 * indexes its R2S_SESSION_MAX_TOPICS arrays by accessor index: the arrays
 * cover every bindable count only when the bindable maximum fits them. */
typedef char r2s_topic_max_within_session_max[(R2S_TOPIC_MAX_COUNT <= R2S_SESSION_MAX_TOPICS) ? 1 : -1];

static struct {
    uint8_t state;
    const char *node_name;
    uint8_t status[R2S_SESSION_MAX_TOPICS];
    uint8_t tx_seq[R2S_SESSION_MAX_TOPICS];
    uint8_t rx_expected[R2S_SESSION_MAX_TOPICS];
    uint16_t rx_lost[R2S_SESSION_MAX_TOPICS];
    uint8_t tx_seq_ctrl;
    uint8_t tx_seq_hb;
    uint32_t boot_id;
    uint32_t last_host_tick;
    uint32_t last_hello_tick;
    uint32_t last_hb_tick;
    uint32_t last_nack_tick;
    uint8_t heard;
} session = {.node_name = ""};

static uint32_t ms_to_ticks(uint32_t ms)
{
    /* On 16-bit ints (AVR) ms * R2S_TICK_HZ would overflow, so the product is
     * 32-bit: at most 2000 * 10^6, which fits. */
    return (ms * (uint32_t)R2S_TICK_HZ) / 1000u;
}

static uint8_t varint_len(uint16_t value)
{
    uint8_t out[R2S_VARINT_MAX_BYTES];

    return r2s_varint_encode(value, out);
}

r2s_status_t r2s_session_configure(const char *node_name, const r2s_topic_t *topics, uint8_t topic_count)
{
    size_t name_len;
    uint8_t i;

    if (node_name == NULL || (topics == NULL && topic_count != 0u) || topic_count > R2S_SESSION_MAX_TOPICS) {
        return R2S_ERR_BOUNDS;
    }
    name_len = strlen(node_name);
    if (name_len > R2S_VARINT_MAX) {
        return R2S_ERR_BOUNDS;
    }
    /* 17 fixed bytes, the length varint and the name: exact, so names of 128
     * bytes or more, whose varint takes two bytes, fit exactly. The sums run
     * in 32 bits, because on a 16-bit size_t a name near 64 KB would wrap a
     * size_t sum back under the limit. */
    if ((uint32_t)17u + (uint32_t)varint_len((uint16_t)name_len) + (uint32_t)name_len >
        (uint32_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD)) {
        return R2S_ERR_BOUNDS;
    }
    for (i = 0u; i < topic_count; i++) {
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
        need = (uint32_t)12u + (uint32_t)varint_len((uint16_t)type_len) + (uint32_t)type_len +
               (uint32_t)varint_len((uint16_t)topic_len) + (uint32_t)topic_len;
        if (need > (uint32_t)(R2S_MAX_FRAME - R2S_TX_OVERHEAD)) {
            return R2S_ERR_BOUNDS;
        }
    }
    if (r2s_topics_configure(topics, topic_count) != R2S_OK) {
        return R2S_ERR_BOUNDS;
    }
    session.node_name = node_name;
    return R2S_OK;
}

uint8_t r2s_session_find_topic(uint8_t topic_id)
{
    return r2s_topic_find(topic_id);
}

uint8_t r2s_session_topic_flags(uint8_t index)
{
    return r2s_topic_get(index)->flags;
}

uint8_t r2s_session_topic_status(uint8_t index)
{
    return session.status[index];
}

uint8_t r2s_session_tx_seq(uint8_t index)
{
    return session.tx_seq[index];
}

void r2s_session_advance_tx_seq(uint8_t index)
{
    session.tx_seq[index] = (uint8_t)(session.tx_seq[index] + 1u);
}

uint16_t r2s_session_note_rx_seq(uint8_t index, uint8_t seq)
{
    /* Both differences wrap mod 256 in uint8_t, which is the count [FRM-15]
     * names; the widening sum keeps every gap. */
    session.rx_lost[index] += (uint16_t)(uint8_t)(seq - session.rx_expected[index]);
    session.rx_expected[index] = (uint8_t)(seq + 1u);
    return session.rx_lost[index];
}

uint16_t r2s_session_rx_lost(uint8_t index)
{
    return session.rx_lost[index];
}

static void enter_announcing(uint32_t now)
{
    uint8_t i;

    session.state = ANNOUNCING;
    session.heard = 0u;
    for (i = 0u; i < R2S_SESSION_MAX_TOPICS; i++) {
        session.status[i] = 0xFFu;
    }
    /* The seq counters restart with the handshake ([SES-11]); the lost count
     * stays, since it counts frames since boot ([FRM-15]). */
    for (i = 0u; i < r2s_topic_count(); i++) {
        session.tx_seq[i] = 0u;
        session.rx_expected[i] = 0u;
    }
    session.last_hello_tick = now;
    session.last_nack_tick = now - ms_to_ticks(HELLO_NACK_MS);
}

static void enter_connected(uint32_t now)
{
    session.state = CONNECTED;
    session.tx_seq_ctrl = 0u;
    session.tx_seq_hb = 0u;
    session.last_hb_tick = now;
}

static void put_byte(uint8_t byte)
{
    (void)r2s_tx_byte(byte);
}

static void put_u16(uint16_t value)
{
    put_byte((uint8_t)(value & 0xFFu));
    put_byte((uint8_t)((value >> 8u) & 0xFFu));
}

static void put_u32(uint32_t value)
{
    put_byte((uint8_t)(value & 0xFFu));
    put_byte((uint8_t)((value >> 8u) & 0xFFu));
    put_byte((uint8_t)((value >> 16u) & 0xFFu));
    put_byte((uint8_t)((value >> 24u) & 0xFFu));
}

static void put_varint(uint16_t value)
{
    uint8_t out[R2S_VARINT_MAX_BYTES];
    uint8_t n = r2s_varint_encode(value, out);
    uint8_t i;

    for (i = 0u; i < n; i++) {
        put_byte(out[i]);
    }
}

static void put_string(const char *text)
{
    /* configure() bounds every name, so the cast cannot truncate. */
    uint16_t len = (uint16_t)strlen(text);
    const uint8_t *bytes = (const uint8_t *)text;
    uint16_t i;

    put_varint(len);
    for (i = 0u; i < len; i++) {
        put_byte(bytes[i]);
    }
}

/* The HELLO payload is 17 fixed bytes, the name length varint and the name.
 * configure() bounds the name, so the casts cannot truncate. */
static uint16_t hello_len(void)
{
    size_t name_len = strlen(session.node_name);
    uint32_t len = (uint32_t)17u + (uint32_t)varint_len((uint16_t)name_len) + (uint32_t)name_len;

    return (uint16_t)len;
}

static void send_hello(void)
{
    if (r2s_tx_begin(0u, session.tx_seq_ctrl, hello_len()) != R2S_OK) {
        return;
    }
    put_byte(OP_HELLO);
    put_byte(R2S_PROTOCOL_VERSION);
    put_byte(0x00u);
    put_u32(session.boot_id);
    put_u16(R2S_MAX_FRAME);
    put_u16(R2S_FRAMER_RX_CAPACITY);
    put_u32(R2S_TICK_HZ);
    put_byte(R2S_FEATURE_BITS);
    put_byte(r2s_topic_count());
    put_string(session.node_name);
    if (r2s_tx_end() == R2S_OK) {
        session.tx_seq_ctrl++;
    }
}

static void send_topic(uint8_t index)
{
    const r2s_topic_t *topic = r2s_topic_get(index);
    uint16_t type_len = (uint16_t)strlen(topic->type_name);
    uint16_t topic_len = (uint16_t)strlen(topic->topic_name);
    uint16_t len = (uint16_t)(12u + varint_len(type_len) + type_len + varint_len(topic_len) + topic_len);
    uint8_t i;

    if (r2s_tx_begin(0u, session.tx_seq_ctrl, len) != R2S_OK) {
        return;
    }
    put_byte(OP_TOPIC);
    put_byte(index);
    put_byte(topic->topic_id);
    put_byte(topic->flags);
    for (i = 0u; i < 8u; i++) {
        put_byte(topic->type_hash[i]);
    }
    put_string(topic->type_name);
    put_string(topic->topic_name);
    if (r2s_tx_end() == R2S_OK) {
        session.tx_seq_ctrl++;
    }
}

static void send_heartbeat(void)
{
    const r2s_framer_counters_t *count = r2s_framer_counters();

    if (r2s_tx_begin(4u, session.tx_seq_hb, 8u) != R2S_OK) {
        return;
    }
    put_u16(count->rx_frames);
    put_u16(count->crc_errors);
    put_u16(count->rx_overflows);
    put_u16(count->dropped_frames);
    if (r2s_tx_end() == R2S_OK) {
        session.tx_seq_hb++;
    }
}

static void refuse(void)
{
    r2s_framer_count_refused();
}

/* Answers a non-handshake host frame with HELLO, at most once per 100 ms
 * (SES-3). Solicited answers do not go through here and are never limited. */
static void answer_with_hello(uint32_t now)
{
    if ((uint32_t)(now - session.last_nack_tick) >= ms_to_ticks(HELLO_NACK_MS)) {
        session.last_nack_tick = now;
        send_hello();
    }
}

static void on_control(uint32_t now, const uint8_t *payload, uint16_t len)
{
    uint8_t i;

    if (len == 0u) {
        refuse();
        return;
    }
    switch (payload[0]) {
    case OP_HELLO_REQ:
        if (len != 2u) {
            refuse();
            return;
        }
        enter_announcing(now);
        send_hello();
        break;
    case OP_TOPIC_REQ:
        if (len != 2u || payload[1] >= r2s_topic_count()) {
            refuse();
            return;
        }
        send_topic(payload[1]);
        break;
    case OP_TOPIC_STATUS:
        if (len != 3u || payload[2] > 3u) {
            refuse();
            return;
        }
        for (i = 0u; i < r2s_topic_count(); i++) {
            if (r2s_topic_get(i)->topic_id == payload[1]) {
                session.status[i] = payload[2];
                return;
            }
        }
        refuse();
        break;
    case OP_READY:
        if (len != 1u) {
            refuse();
            return;
        }
        if (session.state == ANNOUNCING) {
            enter_connected(now);
        }
        break;
    default:
        refuse();
        break;
    }
}

static void on_frame(const r2s_frame_t *frame)
{
    uint8_t topic = frame->data[0];
    const uint8_t *payload = frame->data + 2u;
    uint16_t payload_len = (uint16_t)(frame->len - 4u);

    /* Every frame the framer accepted proves the host is alive, whichever
     * channel it arrived on and whether the session refuses it. */
    session.last_host_tick = frame->tick;
    if (topic == 0u) {
        on_control(frame->tick, payload, payload_len);
    } else if (topic == 4u) {
        if (payload_len != 0u) {
            refuse();
        } else if (session.state == ANNOUNCING) {
            answer_with_hello(frame->tick);
        }
    } else if (topic <= 6u) {
        /* The channels later tickets own (time, log, parameters, error
         * report, reliable ack): accepted, and answered in ANNOUNCING like
         * any other non-handshake frame, otherwise ignored. */
        if (session.state == ANNOUNCING) {
            answer_with_hello(frame->tick);
        }
    } else if (topic >= 16u) {
#ifdef R2S_WITH_CODEC
        /* A configured topic dispatches while connected; in ANNOUNCING the
         * frame is answered like any other non-handshake frame ([SES-3]). */
        if (session.state == CONNECTED) {
            r2s_codec_dispatch(topic, frame->data[1], payload, payload_len);
        } else {
            answer_with_hello(frame->tick);
        }
#else
        /* A user topic has no consumer until the codec lands; it is accepted
         * and ignored, and in ANNOUNCING answered so the host notices. */
        if (session.state == ANNOUNCING) {
            answer_with_hello(frame->tick);
        }
#endif
    } else {
        /* Reserved IDs are ignored without counting and without a reply, ahead
         * of the ANNOUNCING rule (SYS-4 over SES-3). */
    }
    if (session.state == ANNOUNCING) {
        session.heard = 1u;
    }
}

void r2s_init(void)
{
    uint32_t now = r2s_port_tick();

    session.boot_id = r2s_port_boot_id();
    session.tx_seq_ctrl = 0u;
    session.tx_seq_hb = 0u;
    enter_announcing(now);
    send_hello();
}

void r2s_spin_some(void)
{
    uint16_t n = 0u;
    r2s_frame_t frame;
    uint32_t now;

    while (n < R2S_SPIN_MAX_FRAMES && r2s_framer_poll(&frame) != 0u) {
        on_frame(&frame);
        r2s_framer_release();
        n++;
    }
    now = r2s_port_tick();
    if (session.state == CONNECTED &&
        (uint32_t)(now - session.last_host_tick) >= ms_to_ticks(R2S_LINK_TIMEOUT_MS)) {
        enter_announcing(now);
    }
    if (session.state == ANNOUNCING) {
        if (session.heard == 0u &&
            (uint32_t)(now - session.last_hello_tick) >= ms_to_ticks(HELLO_REPEAT_MS)) {
            session.last_hello_tick = now;
            send_hello();
        }
    } else if ((uint32_t)(now - session.last_hb_tick) >= ms_to_ticks(R2S_HEARTBEAT_MS)) {
        session.last_hb_tick = now;
        send_heartbeat();
    }
}

uint8_t r2s_connected(void)
{
    return session.state;
}

#ifdef R2S_TESTING
void r2s_session_test_reset(void)
{
    uint8_t i;

    session.state = ANNOUNCING;
    session.node_name = "";
    r2s_topics_test_reset();
    for (i = 0u; i < R2S_SESSION_MAX_TOPICS; i++) {
        session.status[i] = 0xFFu;
        session.tx_seq[i] = 0u;
        session.rx_expected[i] = 0u;
        session.rx_lost[i] = 0u;
    }
    session.tx_seq_ctrl = 0u;
    session.tx_seq_hb = 0u;
    session.boot_id = 0u;
    session.last_host_tick = 0u;
    session.last_hello_tick = 0u;
    session.last_hb_tick = 0u;
    session.last_nack_tick = 0u;
    session.heard = 0u;
}
#endif
