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
    static r2s_param_t null_name[] = {
        {NULL, R2S_PARAM_BOOL, 0u, 0u, {.b = 1u}, {.b = 0u}},
    };
    static char long_name[600];
    static r2s_param_t long_table[] = {
        {"", R2S_PARAM_BOOL, 0u, 0u, {.b = 1u}, {.b = 0u}},
    };
    static r2s_param_t null_string[] = {
        {"name", R2S_PARAM_STRING, 0u, 7u, {.s = NULL}, {.s = NULL}},
    };
    static r2s_param_t null_value[] = {
        {"name", R2S_PARAM_STRING, 0u, 7u, {.s = "ab"}, {.s = NULL}},
    };
    static char huge_name[65537];
    static r2s_param_t huge_table[] = {
        {"", R2S_PARAM_BOOL, 0u, 0u, {.b = 1u}, {.b = 0u}},
    };
    static char tiny[1];
    static r2s_param_t bad_string[] = {
        {"name", R2S_PARAM_STRING, 0u, sizeof tiny - 1u, {.s = "ab"}, {.s = NULL}},
    };

    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(NULL, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(bad_type, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(bad_flags, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(bad_default, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(null_name, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(null_string, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(null_value, 1));
    memset(huge_name, 'n', sizeof huge_name - 1u);
    huge_name[sizeof huge_name - 1u] = '\0';
    huge_table[0].name = huge_name;
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(huge_table, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_params_configure(NULL, 0));
    memset(long_name, 'n', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = '\0';
    long_table[0].name = long_name;
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_params_configure(long_table, 1));
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
    const uint8_t empty[1] = {0u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    /* Three retries, each with the next frame seq and the same rel seq. The
     * fed heartbeat keeps the link alive; the timers fire while its bytes
     * arrive, so each MCU heartbeat reports the count from before it. */
    for (uint16_t retry = 1u; retry <= 3u; retry++) {
        uint8_t want[8] = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

        fake_port_advance(500);
        feed_host(4, (uint8_t)(retry - 1u), empty, 0);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(2u + 2u * retry), fake_port_tx_frame_count());
        assert_tx((uint16_t)(2u * retry), 3, (uint8_t)retry, status, sizeof status);
        want[0] = (uint8_t)(retry + 1u);
        assert_tx((uint16_t)(2u * retry + 1u), 4, (uint8_t)(retry - 1u), want, sizeof want);
    }
    /* Spent: no fourth STATUS, and the drop counted with the next heartbeat,
     * which still reports the count from before the fed one. */
    {
        uint8_t want[8] = {5u, 0u, 0u, 0u, 0u, 0u, 1u, 0u};

        fake_port_advance(500);
        feed_host(4, 3, empty, 0);
        TEST_ASSERT_EQUAL_UINT16(9, fake_port_tx_frame_count());
        assert_tx(8, 4, 3, want, sizeof want);
    }
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
    /* Four heartbeats and no STATUS: the ack cleared the outstanding one.
     * The fed heartbeats keep the link alive across the 2000 ms. */
    {
        const uint8_t empty[1] = {0u};

        for (uint8_t step = 0u; step < 4u; step++) {
            fake_port_advance(500);
            feed_host(4, step, empty, 0);
        }
    }
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
    fake_port_tx_clear();
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

    /* Past the first second, so the rate-limited busy report is due. */
    fake_port_set_tick(5000);
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
    static const uint8_t bad_varint[] = {0x03u, 0x00u, 0x02u, 0x80u};
    static const uint8_t short_string[] = {0x03u, 0x01u, 0x02u, 0x04u, 'h', 'i'};

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
    feed_host(3, 4, bad_varint, sizeof bad_varint);
    feed_host(3, 5, short_string, sizeof short_string);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: PAR-5 */
void test_params_set_with_short_int_value_answers_bad_length(void)
{
    /* SET rel 0, index 1, no value bytes: an i64 needs 8. */
    const uint8_t set[] = {0x03u, 0x00u, 0x01u};
    const uint8_t ack[] = {0x03u, 0x00u};
    const uint8_t status[] = {0x04u, 0x00u, 0x01u, 0x02u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, set, sizeof set);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(0, 6, 0, ack, sizeof ack);
    assert_tx(1, 3, 0, status, sizeof status);
}

/* covers: PAR-5 */
void test_params_check_set_reports_unknown_index(void)
{
    static const uint8_t value[] = {0x00u};

    configure();
    TEST_ASSERT_EQUAL_INT(R2S_PARAM_UNKNOWN_INDEX, r2s_params_check_set(9, value, sizeof value));
}

/* covers: PAR-2 */
void test_params_declare_len_reports_zero_past_the_table(void)
{
    configure();
    TEST_ASSERT_EQUAL_UINT16(0u, r2s_params_declare_len(9));
}

/* covers: PAR-6 */
void test_params_store_applies_int_value(void)
{
    static const uint8_t seven[] = {0x07u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

    configure();
    r2s_params_store(1, seven, sizeof seven);
    TEST_ASSERT_TRUE(r2s_params_get(1)->value.i == 7);
}

/* covers: PAR-5 */
void test_params_set_answers_unknown_readonly_bad_length_or_range(void)
{
    /* Unknown index. */
    const uint8_t unknown[] = {0x03u, 0x00u, 0x09u, 0x00u};
    const uint8_t unknown_status[] = {0x04u, 0x00u, 0x09u, 0x01u};
    const uint8_t ack0[] = {0x03u, 0x00u};
    /* Eight value bytes for a bool: bad length. */
    const uint8_t bad_length[] = {0x03u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u,
                                  0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    const uint8_t bad_length_status[] = {0x04u, 0x01u, 0x00u, 0x02u};
    const uint8_t ack1[] = {0x03u, 0x01u};
    /* A SET to the read-only limit. */
    const uint8_t readonly[] = {0x03u, 0x02u, 0x01u, 0x07u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    const uint8_t readonly_status[] = {0x04u, 0x02u, 0x01u, 0x03u};
    const uint8_t ack2[] = {0x03u, 0x02u};
    /* Eight bytes into the seven-byte string: out of range. */
    const uint8_t too_long[] = {0x03u, 0x03u, 0x02u, 0x08u, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h'};
    const uint8_t too_long_status[] = {0x04u, 0x03u, 0x02u, 0x04u};

    configure();
    connect();
    fake_port_tx_clear();
    /* Each STATUS is acked before the next SET, the way a conforming host
     * answers every reliable frame. */
    feed_host(3, 0, unknown, sizeof unknown);
    assert_tx(1, 3, 0, unknown_status, sizeof unknown_status);
    feed_host(6, 0, ack0, sizeof ack0);
    feed_host(3, 1, bad_length, sizeof bad_length);
    assert_tx(3, 3, 1, bad_length_status, sizeof bad_length_status);
    feed_host(6, 1, ack1, sizeof ack1);
    feed_host(3, 2, readonly, sizeof readonly);
    assert_tx(5, 3, 2, readonly_status, sizeof readonly_status);
    feed_host(6, 2, ack2, sizeof ack2);
    feed_host(3, 3, too_long, sizeof too_long);
    assert_tx(7, 3, 3, too_long_status, sizeof too_long_status);
    /* No Unity 64-bit assert on 32-bit builds: compare directly. */
    TEST_ASSERT_TRUE(r2s_params_get(1)->value.i == 7);
    TEST_ASSERT_EQUAL_STRING("ab", r2s_params_get(2)->value.s);
}

/* covers: PAR-5 */
void test_params_readonly_string_over_bound_earns_readonly(void)
{
    static char buf[4];
    static r2s_param_t ro_table[] = {
        {"mode", R2S_PARAM_STRING, R2S_PARAM_READONLY, sizeof buf - 1u, {.s = "ab"}, {.s = NULL}},
    };
    /* Four text bytes into a three-byte read-only string: read-only wins
     * over range, the documented verdict order. */
    static const uint8_t over[] = {0x04u, 'a', 'b', 'c', 'd'};

    buf[0] = '\0';
    ro_table[0].value.s = buf;
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_params_configure(ro_table, 1));
    TEST_ASSERT_EQUAL_INT(R2S_PARAM_READONLY_STATUS, r2s_params_check_set(0, over, sizeof over));
}

/* covers: REL-2 */
/* covers: PAR-1, PAR-2, PAR-6 */
void test_params_declare_answers_int_and_string_types(void)
{
    const uint8_t req_one[] = {0x01u, 0x01u};
    /* DECLARE index 1, i64, read-only, name "limit", default 7. */
    const uint8_t declare_i64[] = {0x02u, 0x01u, 0x01u, 0x01u, 0x05u, 'l', 'i', 'm', 'i', 't',
                                   0x07u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    const uint8_t req_two[] = {0x01u, 0x02u};
    /* DECLARE index 2, string, no flags, name "name", default "ab". */
    const uint8_t declare_string[] = {0x02u, 0x02u, 0x03u, 0x00u, 0x04u, 'n', 'a', 'm', 'e',
                                      0x02u, 'a', 'b'};

    configure();
    feed_host(3, 0, req_one, sizeof req_one);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(1, 3, 0, declare_i64, sizeof declare_i64);
    feed_host(3, 1, req_two, sizeof req_two);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    assert_tx(2, 3, 1, declare_string, sizeof declare_string);
}

/* covers: PAR-1, PAR-2, PAR-5, PAR-6 */
void test_params_f64_declare_streams_eight_bytes(void)
{
    static r2s_param_t f64_table[] = {
        {"gain", R2S_PARAM_F64, 0u, 0u, {.f = 1.5}, {.f = 0}},
    };
    /* DECLARE index 0, f64, no flags, name "gain", default 1.5. */
    const uint8_t declare[] = {0x02u, 0x00u, 0x02u, 0x00u, 0x04u, 'g', 'a', 'i', 'n',
                               0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0xF8u, 0x3Fu};
    const uint8_t req[] = {0x01u, 0x00u};
    static const uint8_t two_point_five[] = {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x04u,
                                             0x40u};
    static const uint8_t short_value[] = {0x00u, 0x00u};

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_params_configure(f64_table, 1));
    r2s_init();
    feed_host(3, 0, req, sizeof req);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(1, 3, 0, declare, sizeof declare);
    TEST_ASSERT_EQUAL_INT(R2S_PARAM_APPLIED, r2s_params_check_set(0, two_point_five, 8));
    TEST_ASSERT_EQUAL_INT(R2S_PARAM_BAD_TYPE_OR_LENGTH, r2s_params_check_set(0, short_value, 2));
    r2s_params_store(0, two_point_five, sizeof two_point_five);
    TEST_ASSERT_EQUAL_MEMORY(two_point_five, &f64_table[0].value.f, sizeof two_point_five);
}

void test_reliable_second_set_waits_behind_the_first(void)
{
    const uint8_t first[] = {0x03u, 0x00u, 0x00u, 0x00u};
    const uint8_t second[] = {0x03u, 0x01u, 0x02u, 0x02u, 'h', 'i'};
    const uint8_t ack_first[] = {0x03u, 0x00u};
    const uint8_t ack_second[] = {0x03u, 0x01u};
    const uint8_t second_status[] = {0x04u, 0x01u, 0x02u, 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(3, 0, first, sizeof first);
    /* The second SET applies while the first STATUS is still outstanding,
     * but its STATUS waits behind it: one payload per direction. */
    feed_host(3, 1, second, sizeof second);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    assert_tx(2, 6, 1, ack_second, sizeof ack_second);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_params_get(0)->value.b);
    TEST_ASSERT_EQUAL_STRING("hi", r2s_params_get(2)->value.s);
    /* The first ack releases the pending STATUS with its own rel seq. */
    feed_host(6, 0, ack_first, sizeof ack_first);
    TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
    assert_tx(3, 3, 1, second_status, sizeof second_status);
    feed_host(6, 1, ack_second, sizeof ack_second);
    TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
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

/* covers: PAR-7, PAR-8 */
void test_params_announcing_blocked_link_sends_nothing(void)
{
    const uint8_t req[] = {0x01u, 0x00u};
    const uint8_t req_unknown[] = {0x01u, 0x09u};

    configure();
    fake_port_tx_clear();
    fake_port_set_tx_free(0);
    feed_host(3, 0, req, sizeof req);
    feed_host(3, 1, req_unknown, sizeof req_unknown);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
}

/* covers: REL-4, PAR-7 */
void test_params_announcing_answers_non_handshake_frames_with_hello(void)
{
    const uint8_t ack[] = {0x03u, 0x00u};
    const uint8_t short_req[] = {0x01u};
    const uint8_t stray_status[] = {0x04u, 0x00u};

    configure();
    fake_port_tx_clear();
    feed_host(6, 0, ack, sizeof ack);
    fake_port_advance(100);
    feed_host(3, 1, short_req, sizeof short_req);
    fake_port_advance(100);
    feed_host(3, 2, stray_status, sizeof stray_status);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
}

/* covers: REL-2, REL-3 */
void test_reliable_ack_for_old_rel_leaves_the_outstanding_set(void)
{
    const uint8_t set[] = {0x03u, 0x00u, 0x00u, 0x00u};
    const uint8_t stray[] = {0x03u, 0x09u};
    const uint8_t empty[] = {0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    const uint8_t status[] = {0x04u, 0x00u, 0x00u, 0x00u};
    const uint8_t want[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

    configure();
    connect();
    fake_port_tx_clear();
    feed_host(6, 0, stray, sizeof stray);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    feed_host(3, 0, set, sizeof set);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    feed_host(6, 1, stray, sizeof stray);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    fake_port_advance(500);
    feed_host(4, 0, empty, sizeof empty);
    TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
    assert_tx(2, 3, 1, status, sizeof status);
    assert_tx(3, 4, 0, want, sizeof want);
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
