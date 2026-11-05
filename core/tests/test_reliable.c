/* The MCU side of the parameters channel and the reliable channel: the table
 * it announces, the SET frames it applies and the acks it answers with. */
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_params.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

static uint8_t wire[WIRE_MAX];

static const r2s_topic_t one_topic[] = {
    {0x10u, 0x00u, {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u}, "std_msgs/msg/Float32", "float"},
};

/* The announced table: a bool led defaulting to true, a read-only i64 limit
 * and a bounded string name. configure() copies each default live. */
static char name_buf[8];
static r2s_param_t table[] = {
    {"led", R2S_PARAM_BOOL, 0u, 0u, {.b = 1u}, {.b = 0u}},
    {"limit", R2S_PARAM_I64, R2S_PARAM_READONLY, 0u, {.i = 7}, {.i = 0}},
    {"name", R2S_PARAM_STRING, 0u, sizeof name_buf - 1u, {.s = "ab"}, {.s = NULL}},
};

static void configure(void)
{
    name_buf[0] = '\0';
    table[2].value.s = name_buf;
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_params_configure(table, 3));
    TEST_ASSERT_EQUAL_UINT8(3, r2s_params_count());
    r2s_init();
}

/* Queues bytes one at a time, spinning after each, as a slow link delivers
 * them. */
static void feed_slow(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0u; i < len; i++) {
        fake_port_queue_rx(&data[i], 1);
        r2s_spin_some();
    }
}

/* Builds a host frame and feeds it byte by byte. */
static void feed_host(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
    uint16_t wire_len = r2s_test_frame(wire, topic_id, seq, payload, len);

    feed_slow(wire, wire_len);
}

/* Checks the index-th captured frame against the wire form of its topic, seq
 * and payload, the same comparison the frames conformance test makes. */
static void assert_tx(uint16_t index, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
                      uint16_t payload_len)
{
    static uint8_t expected[WIRE_MAX];
    char label[64];
    uint16_t encoded_len;
    const uint8_t *encoded;
    uint16_t expected_len = r2s_test_frame(expected, topic_id, seq, payload, payload_len);

    snprintf(label, sizeof label, "tx frame %u", index);
    encoded = fake_port_tx_frame(index, &encoded_len);
    TEST_ASSERT_NOT_NULL_MESSAGE(encoded, label);
    /* The captured frame stops at its delimiter, which the built wire keeps. */
    R2S_ASSERT_BYTES(expected, (uint16_t)(expected_len - 1u), encoded, encoded_len, label);
}

/* Moves to CONNECTED the way the handshake does: READY with a nonzero
 * token, answered in ANNOUNCING. */
static void connect(void)
{
    const uint8_t ready[] = {0x06u, 0x01u, 0x00u, 0x00u, 0x00u};

    feed_host(0, 0, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
}

/* covers: PAR-2, PAR-4 */
void test_params_configure_refuses_a_table_it_cannot_announce(void)
{
    static r2s_param_t bad_type[] = {
        {"led", 9u, 0u, 0u, {.b = 1u}, {.b = 0u}},
    };
    static r2s_param_t bad_flags[] = {
        {"led", R2S_PARAM_BOOL, 0x02u, 0u, {.b = 1u}, {.b = 0u}},
    };
    static r2s_param_t bad_default[] = {
        {"led", R2S_PARAM_BOOL, 0u, 0u, {.b = 5u}, {.b = 0u}},
    };
    static char tiny[1];
    static r2s_param_t bad_string[] = {
        {"name", R2S_PARAM_STRING, 0u, sizeof tiny - 1u, {.s = "ab"}, {.s = NULL}},
    };

    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(NULL, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(bad_type, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(bad_flags, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(bad_default, 1));
    bad_string[0].value.s = tiny;
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(bad_string, 1));
    TEST_ASSERT_EQUAL_UINT8(0, r2s_params_count());
    TEST_ASSERT_NULL(r2s_params_get(0));
}

/* covers: PAR-1, PAR-2, PAR-6 */
void test_params_req_answers_declare_in_announcing(void)
{
    const uint8_t req[] = {0x01u, 0x00u};
    /* DECLARE index 0, bool, no flags, name "led", default true. */
    const uint8_t declare[] = {0x02u, 0x00u, 0x00u, 0x00u, 0x03u, 'l', 'e', 'd', 0x01u};

    configure();
    feed_host(3, 0, req, sizeof req);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(1, 3, 0, declare, sizeof declare);
}

/* covers: PAR-8, PAR-5 */
void test_params_req_for_unknown_index_answers_status_1_plain(void)
{
    const uint8_t req[] = {0x01u, 0x09u};
    /* A plain reply under the request retry carries rel_seq 0. */
    const uint8_t status[] = {0x04u, 0x00u, 0x09u, 0x01u};

    configure();
    feed_host(3, 0, req, sizeof req);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(1, 3, 0, status, sizeof status);
}

/* covers: REL-1, REL-3, REL-4, PAR-8 */
void test_reliable_set_applies_and_reports_with_ack(void)
{
    /* SET rel 0, index 0, false. */
    const uint8_t set[] = {0x03u, 0x00u, 0x00u, 0x00u};
    const uint8_t ack[] = {0x03u, 0x00u};
    /* STATUS rel 0, index 0, applied. */
    const uint8_t status[] = {0x04u, 0x00u, 0x00u, 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    /* The ack names the channel and the rel_seq of the frame received. */
    assert_tx(0, 6, 0, ack, sizeof ack);
    /* The STATUS rides the reliable channel with its own rel_seq. */
    assert_tx(1, 3, 0, status, sizeof status);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_params_get(0)->value.b);
}

/* covers: REL-3 */
void test_reliable_duplicate_set_is_acked_not_applied(void)
{
    const uint8_t set[] = {0x03u, 0x00u, 0x00u, 0x00u};
    /* Same rel seq, other value: a duplicate the link mangled or resent. */
    const uint8_t resend[] = {0x03u, 0x00u, 0x00u, 0x01u};
    const uint8_t ack[] = {0x03u, 0x00u};
    const uint8_t status[] = {0x04u, 0x00u, 0x00u, 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    feed_host(3, 1, resend, sizeof resend);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    assert_tx(0, 6, 0, ack, sizeof ack);
    assert_tx(1, 3, 0, status, sizeof status);
    /* The duplicate is acked again on the next ack seq; nothing applies. */
    assert_tx(2, 6, 1, ack, sizeof ack);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_params_get(0)->value.b);
}

/* covers: REL-3 */
void test_reliable_future_set_is_acked_not_applied(void)
{
    const uint8_t set[] = {0x03u, 0x05u, 0x00u, 0x00u};
    const uint8_t ack[] = {0x03u, 0x05u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_tx(0, 6, 0, ack, sizeof ack);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_params_get(0)->value.b);
}

/* covers: REL-2, REL-4, HB-7 */
void test_reliable_status_retransmits_then_gives_up_counted(void)
{
    const uint8_t set[] = {0x03u, 0x00u, 0x00u, 0x00u};
    const uint8_t status[] = {0x04u, 0x00u, 0x00u, 0x00u};
    /* rx 2, crc 0, overflow 0, dropped 0: the give-up has not happened yet. */
    const uint8_t heartbeat[] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    /* Dropped 1: the spent STATUS counted [HB-7] well. */
    const uint8_t last_heartbeat[] = {0x02u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    /* Three retries, each with the next frame seq and the same rel seq,
     * beside the heartbeat each 500 ms step also carries. */
    for (uint16_t retry = 1u; retry <= 3u; retry++) {
        fake_port_advance(500);
        r2s_spin_some();
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(2u + 2u * retry), fake_port_tx_frame_count());
        assert_tx((uint16_t)(2u * retry), 3, (uint8_t)retry, status, sizeof status);
        assert_tx((uint16_t)(2u * retry + 1u), 4, (uint8_t)(retry - 1u), heartbeat, sizeof heartbeat);
    }
    /* Spent: no fourth STATUS, and the drop counted with the next heartbeat. */
    fake_port_advance(500);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(9, fake_port_tx_frame_count());
    assert_tx(8, 4, 3, last_heartbeat, sizeof last_heartbeat);
}

/* covers: REL-2, REL-1 */
void test_reliable_host_ack_stops_the_retry(void)
{
    const uint8_t set[] = {0x03u, 0x00u, 0x00u, 0x00u};
    const uint8_t ack[] = {0x03u, 0x00u};
    const uint8_t next[] = {0x03u, 0x01u, 0x00u, 0x01u};
    const uint8_t next_status[] = {0x04u, 0x01u, 0x00u, 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    feed_host(6, 0, ack, sizeof ack);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    /* Four heartbeats and no STATUS: the ack cleared the outstanding one. */
    fake_port_advance(2000);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(6, fake_port_tx_frame_count());
    /* The next SET moves the rel seq on. */
    feed_host(3, 1, next, sizeof next);
    {
        const uint8_t next_ack[] = {0x03u, 0x01u};
        assert_tx(6, 6, 1, next_ack, sizeof next_ack);
    }
    assert_tx(7, 3, 1, next_status, sizeof next_status);
}

/* covers: REL-5 */
void test_reliable_rehandshake_drops_reliable_state(void)
{
    const uint8_t set[] = {0x03u, 0x00u, 0x00u, 0x00u};
    const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
    const uint8_t ready[] = {0x06u, 0x02u, 0x00u, 0x00u, 0x00u};
    const uint8_t status[] = {0x04u, 0x00u, 0x00u, 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    /* A HELLO_REQ re-announces: the outstanding STATUS never goes out. */
    feed_host(0, 1, hello_req, sizeof hello_req);
    fake_port_advance(1500);
    r2s_spin_some();
    for (uint16_t i = 0u; i < fake_port_tx_frame_count(); i++) {
        uint16_t len = 0u;
        const uint8_t *frame = fake_port_tx_frame(i, &len);

        TEST_ASSERT_NOT_NULL(frame);
        TEST_ASSERT_TRUE(len < 3u || frame[0] != 3u);
    }
    /* The new handshake restarts the rel seq at 0. */
    feed_host(0, 2, ready, sizeof ready);
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(1, 3, 0, status, sizeof status);
}

/* covers: REL-6 */
void test_reliable_blocked_ack_reports_busy_transmitter(void)
{
    const uint8_t set[] = {0x03u, 0x00u, 0x00u, 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    /* No space for even one frame: the ack cannot go out. */
    fake_port_set_tx_free(0);
    feed_host(3, 0, set, sizeof set);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_take_overruns());
    fake_port_set_tx_free(0xFFFFu);
    /* The busy transmitter is reported the way code 6 reports it, and the
     * queued STATUS goes out once the link takes bytes. */
    fake_port_advance(1);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    {
        /* code 6, topic 3, count 1, field 0xFF. */
        const uint8_t busy[] = {0x06u, 0x03u, 0x01u, 0x00u, 0xFFu};
        const uint8_t status[] = {0x04u, 0x00u, 0x00u, 0x00u};
        assert_tx(0, 5, 0, busy, sizeof busy);
        assert_tx(1, 3, 0, status, sizeof status);
    }
}

/* covers: SYS-5, REL-4, REL-1 */
void test_reliable_malformed_frames_are_refused_not_acked(void)
{
    const uint8_t short_set[] = {0x03u, 0x00u};
    const uint8_t bad_bool[] = {0x03u, 0x00u, 0x00u, 0x05u};
    const uint8_t short_req[] = {0x01u};
    const uint8_t stray_status[] = {0x04u, 0x00u, 0x00u, 0x00u};
    const uint8_t short_ack[] = {0x03u};
    const uint8_t wrong_channel_ack[] = {0x05u, 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, short_set, sizeof short_set);
    feed_host(3, 1, bad_bool, sizeof bad_bool);
    feed_host(3, 2, short_req, sizeof short_req);
    feed_host(3, 3, stray_status, sizeof stray_status);
    /* An ID 6 frame is never acked and never rides the reliable channel. */
    feed_host(6, 0, short_ack, sizeof short_ack);
    feed_host(6, 1, wrong_channel_ack, sizeof wrong_channel_ack);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: PAR-5 */
void test_params_set_answers_unknown_readonly_bad_length_or_range(void)
{
    /* Unknown index. */
    const uint8_t unknown[] = {0x03u, 0x00u, 0x09u, 0x00u};
    const uint8_t unknown_status[] = {0x04u, 0x00u, 0x09u, 0x01u};
    /* Eight value bytes for a bool: bad length. */
    const uint8_t bad_length[] = {0x03u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u,
                                  0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    const uint8_t bad_length_status[] = {0x04u, 0x01u, 0x00u, 0x02u};
    /* A SET to the read-only limit. */
    const uint8_t readonly[] = {0x03u, 0x02u, 0x01u, 0x07u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    const uint8_t readonly_status[] = {0x04u, 0x02u, 0x01u, 0x03u};
    /* Three bytes into a two-byte string: out of range. */
    const uint8_t too_long[] = {0x03u, 0x03u, 0x02u, 0x03u, 'a', 'b', 'c'};
    const uint8_t too_long_status[] = {0x04u, 0x03u, 0x02u, 0x04u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, unknown, sizeof unknown);
    assert_tx(1, 3, 0, unknown_status, sizeof unknown_status);
    feed_host(3, 1, bad_length, sizeof bad_length);
    assert_tx(3, 3, 1, bad_length_status, sizeof bad_length_status);
    feed_host(3, 2, readonly, sizeof readonly);
    assert_tx(5, 3, 2, readonly_status, sizeof readonly_status);
    feed_host(3, 3, too_long, sizeof too_long);
    assert_tx(7, 3, 3, too_long_status, sizeof too_long_status);
    TEST_ASSERT_EQUAL_INT64(7, r2s_params_get(1)->value.i);
}

/* covers: PAR-3 */
void test_params_string_set_applies_and_rejects_nul(void)
{
    /* SET rel 0, index 2, "hi". */
    const uint8_t set[] = {0x03u, 0x00u, 0x02u, 0x02u, 'h', 'i'};
    const uint8_t status[] = {0x04u, 0x00u, 0x02u, 0x00u};
    /* An embedded NUL breaks the type rule: refused, not answered. */
    const uint8_t nul[] = {0x03u, 0x01u, 0x02u, 0x02u, 'h', 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    assert_tx(1, 3, 0, status, sizeof status);
    TEST_ASSERT_EQUAL_STRING("hi", r2s_params_get(2)->value.s);
    feed_host(3, 1, nul, sizeof nul);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    TEST_ASSERT_EQUAL_STRING("hi", r2s_params_get(2)->value.s);
}

void test_params_set_ignored_in_announcing(void)
{
    const uint8_t set[] = {0x03u, 0x00u, 0x00u, 0x00u};

    configure();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    /* Answered like any other non-handshake frame, otherwise ignored. */
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    TEST_ASSERT_EQUAL_UINT8(1, r2s_params_get(0)->value.b);
}
