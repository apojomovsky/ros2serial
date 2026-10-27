/* The MCU session through the fake port: boot, a full handshake byte by byte,
 * timeouts, re-announce and the heartbeat counters. */
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"
#include "r2s/r2s_varint.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

static uint8_t wire[WIRE_MAX];

/* The announced table for the one-topic tests: the shape of
 * spec/vectors/frames/topic.yaml, so the TOPIC the core sends carries the
 * vector's payload. */
static const r2s_session_topic_t one_topic[] = {
    {0x10u, 0x03u, {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u}, "std_msgs/msg/Float32", "float"},
};

static const r2s_session_topic_t two_topics[] = {
    {0x10u,
     0x00u,
     {0x10u, 0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u},
     "std_msgs/msg/Float32",
     "battery"},
    {0x11u,
     0x01u,
     {0x20u, 0x21u, 0x22u, 0x23u, 0x24u, 0x25u, 0x26u, 0x27u},
     "geometry_msgs/msg/Twist",
     "cmd_vel"},
};

#if DBL_MANT_DIG == FLT_MANT_DIG
#define EXPECTED_FEATURES 0x10u
#else
#define EXPECTED_FEATURES 0x00u
#endif

/* The TOPIC payload the one-topic table announces, byte for byte the payload
 * of spec/vectors/frames/topic.yaml. */
static const uint8_t topic_frame_payload[] = {
    0x04u, 0x00u, 0x10u, 0x03u, 0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x14u,
    's',   't',   'd',   '_',   'm',   's',   'g',   's',   '/',   'm',   's',   'g',   '/',
    'F',   'l',   'o',   'a',   't',   '3',   '2',   0x05u, 'f',   'l',   'o',   'a',   't'};

static void configure(const r2s_session_topic_t *topics, uint8_t count, uint32_t boot_id)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", topics, count));
    fake_port_set_boot_id(boot_id);
    r2s_init();
}

/* Queues bytes one at a time, spinning after each, as a slow link delivers
 * them. */
static void feed_slow(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
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

/* Builds the HELLO payload the configuration announces: the hello vector's
 * shape, with the build's own limits, tick rate and features. */
static uint16_t hello_payload(uint8_t *out, uint32_t boot_id, uint8_t topic_count)
{
    const char *name = "node";
    size_t name_len = strlen(name);
    uint8_t varint[R2S_VARINT_MAX_BYTES];
    uint8_t varint_len = r2s_varint_encode((uint16_t)name_len, varint);
    uint16_t len;

    out[0] = 0x02u;
    out[1] = R2S_PROTOCOL_VERSION;
    out[2] = 0x00u;
    out[3] = (uint8_t)(boot_id & 0xFFu);
    out[4] = (uint8_t)((boot_id >> 8u) & 0xFFu);
    out[5] = (uint8_t)((boot_id >> 16u) & 0xFFu);
    out[6] = (uint8_t)((boot_id >> 24u) & 0xFFu);
    out[7] = (uint8_t)(R2S_MAX_FRAME & 0xFFu);
    out[8] = (uint8_t)((R2S_MAX_FRAME >> 8u) & 0xFFu);
    out[9] = (uint8_t)(R2S_FRAMER_RX_CAPACITY & 0xFFu);
    out[10] = (uint8_t)((R2S_FRAMER_RX_CAPACITY >> 8u) & 0xFFu);
    out[11] = (uint8_t)(R2S_TICK_HZ & 0xFFu);
    out[12] = (uint8_t)((R2S_TICK_HZ >> 8u) & 0xFFu);
    out[13] = (uint8_t)((R2S_TICK_HZ >> 16u) & 0xFFu);
    out[14] = (uint8_t)((R2S_TICK_HZ >> 24u) & 0xFFu);
    out[15] = EXPECTED_FEATURES;
    out[16] = topic_count;
    memcpy(out + 17u, varint, varint_len);
    memcpy(out + 17u + varint_len, name, name_len);
    len = (uint16_t)(17u + varint_len + name_len);
    return len;
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

/* covers: SES-1, CTL-2, CTL-7, CTL-8, CTL-10, FRM-9 */
void test_session_boot_sends_hello_and_announces(void)
{
    uint8_t expected[64];
    uint16_t len;

    configure(one_topic, 1, 0x44332211u);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    len = hello_payload(expected, 0x44332211u, 1);
    assert_tx(0, 0, 0, expected, len);
}

/* covers: CTL-16 */
void test_session_names_of_128_bytes_or_more_travel_whole(void)
{
    static char big_node[201];
    static char big_type[201];
    static char big_topic[201];
    static uint8_t expected[560];
    static r2s_session_topic_t big[1];
    const uint8_t hello_req[] = {0x01u, 0x01u};
    const uint8_t topic_req[] = {0x03u, 0x00u};
    const uint8_t hash[8] = {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u};
    size_t n = 0;
    uint16_t hello_len;
    uint16_t topic_len;

    memset(big_node, 'n', sizeof big_node - 1u);
    memset(big_type, 't', sizeof big_type - 1u);
    memset(big_topic, 'p', sizeof big_topic - 1u);
    big[0].topic_id = 0x10u;
    big[0].flags = 0x02u;
    memcpy(big[0].type_hash, hash, sizeof hash);
    big[0].type_name = big_type;
    big[0].topic_name = big_topic;
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure(big_node, big, 1));
    fake_port_set_boot_id(0x44332211u);
    r2s_init();

    /* The HELLO carries the 200-byte name behind a two-byte varint. */
    expected[n++] = 0x02u;
    expected[n++] = R2S_PROTOCOL_VERSION;
    expected[n++] = 0x00u;
    expected[n++] = 0x11u;
    expected[n++] = 0x22u;
    expected[n++] = 0x33u;
    expected[n++] = 0x44u;
    expected[n++] = (uint8_t)(R2S_MAX_FRAME & 0xFFu);
    expected[n++] = (uint8_t)((R2S_MAX_FRAME >> 8u) & 0xFFu);
    expected[n++] = (uint8_t)(R2S_FRAMER_RX_CAPACITY & 0xFFu);
    expected[n++] = (uint8_t)((R2S_FRAMER_RX_CAPACITY >> 8u) & 0xFFu);
    expected[n++] = (uint8_t)(R2S_TICK_HZ & 0xFFu);
    expected[n++] = (uint8_t)((R2S_TICK_HZ >> 8u) & 0xFFu);
    expected[n++] = (uint8_t)((R2S_TICK_HZ >> 16u) & 0xFFu);
    expected[n++] = (uint8_t)((R2S_TICK_HZ >> 24u) & 0xFFu);
    expected[n++] = EXPECTED_FEATURES;
    expected[n++] = 0x01u;
    n += r2s_varint_encode(200u, expected + n);
    memcpy(expected + n, big_node, 200u);
    n += 200u;
    hello_len = (uint16_t)n;
    assert_tx(0, 0, 0, expected, hello_len);

    /* The TOPIC carries both 200-byte names the same way. */
    feed_host(0, 0, hello_req, sizeof hello_req);
    n = 0;
    expected[n++] = 0x04u;
    expected[n++] = 0x00u;
    expected[n++] = 0x10u;
    expected[n++] = 0x02u;
    memcpy(expected + n, hash, sizeof hash);
    n += sizeof hash;
    n += r2s_varint_encode(200u, expected + n);
    memcpy(expected + n, big_type, 200u);
    n += 200u;
    n += r2s_varint_encode(200u, expected + n);
    memcpy(expected + n, big_topic, 200u);
    n += 200u;
    topic_len = (uint16_t)n;
    feed_host(0, 1, topic_req, sizeof topic_req);
    assert_tx(2, 0, 2, expected, topic_len);
}

/* covers: SES-2, SES-6, SES-9, CTL-4 */
void test_session_a_full_handshake_reaches_connected(void)
{
    const uint8_t hello_req[] = {0x01u, 0x01u};
    const uint8_t topic_req[] = {0x03u, 0x00u};
    const uint8_t topic_status[] = {0x05u, 0x10u, 0x00u};
    const uint8_t ready[] = {0x06u};
    uint8_t expected[64];
    uint16_t len;
    const uint8_t heartbeat[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};

    configure(one_topic, 1, 0x44332211u);
    feed_host(0, 0, hello_req, sizeof hello_req);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    len = hello_payload(expected, 0x44332211u, 1);
    assert_tx(1, 0, 1, expected, len);

    feed_host(0, 1, topic_req, sizeof topic_req);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    assert_tx(2, 0, 2, topic_frame_payload, sizeof topic_frame_payload);

    feed_host(0, 2, topic_status, sizeof topic_status);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());

    feed_host(0, 3, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());

    fake_port_advance(500);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
    assert_tx(3, 4, 0, heartbeat, sizeof heartbeat);
}

/* covers: SES-2 */
void test_session_a_topic_status_rejecting_one_topic_still_connects(void)
{
    const uint8_t hello_req[] = {0x01u, 0x01u};
    const uint8_t topic_req[] = {0x03u, 0x01u};
    const uint8_t ok[] = {0x05u, 0x10u, 0x00u};
    const uint8_t mismatch[] = {0x05u, 0x11u, 0x02u};
    const uint8_t ready[] = {0x06u};

    configure(two_topics, 2, 0x44332211u);
    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, topic_req, sizeof topic_req);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    /* Index, ID, flags, hash and names of the second table entry. */
    {
        const uint8_t announced[] = {0x04u, 0x01u, 0x11u, 0x01u, 0x20u, 0x21u, 0x22u, 0x23u, 0x24u,
                                     0x25u, 0x26u, 0x27u, 0x17u, 'g',   'e',   'o',   'm',   'e',
                                     't',   'r',   'y',   '_',   'm',   's',   'g',   's',   '/',
                                     'm',   's',   'g',   '/',   'T',   'w',   'i',   's',   't',
                                     0x07u, 'c',   'm',   'd',   '_',   'v',   'e',   'l'};
        assert_tx(2, 0, 2, announced, sizeof announced);
    }
    /* The rejection is stored for the codec to gate its publish on; the
     * handshake itself completes. */
    feed_host(0, 2, ok, sizeof ok);
    feed_host(0, 3, mismatch, sizeof mismatch);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    feed_host(0, 4, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
}

/* covers: SES-7, SES-8 */
void test_session_a_link_timeout_returns_to_announcing(void)
{
    const uint8_t hello_req[] = {0x01u, 0x01u};
    const uint8_t ready[] = {0x06u};
    uint8_t expected[64];
    uint16_t len;

    /* Near the wraparound, so the elapsed time stays an unsigned difference. */
    fake_port_set_tick(0xFFFFFC00u);
    configure(one_topic, 1, 0x44332211u);
    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());

    fake_port_advance(1999);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    /* One heartbeat went out while the link was still alive. */
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    {
        const uint8_t heartbeat[] = {0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
        assert_tx(2, 4, 0, heartbeat, sizeof heartbeat);
    }

    fake_port_advance(1);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());

    /* Silence again, so the announce repeats every second. */
    fake_port_advance(999);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    fake_port_advance(1);
    r2s_spin_some();
    len = hello_payload(expected, 0x44332211u, 1);
    /* The READY reset the counters, so the re-announce answers from 0. */
    assert_tx(3, 0, 0, expected, len);
}

/* covers: SES-10 */
void test_session_the_boot_id_is_read_once_per_boot(void)
{
    const uint8_t hello_req[] = {0x01u, 0x01u};
    uint8_t expected[64];
    uint16_t len;

    configure(one_topic, 1, 0xA5A5A5A5u);
    len = hello_payload(expected, 0xA5A5A5A5u, 1);
    assert_tx(0, 0, 0, expected, len);

    /* A changed port value mid-run is not a new boot: the reply keeps the old
     * ID, so the host cannot mistake it for a reset. */
    fake_port_set_boot_id(0xDEADBEEFu);
    feed_host(0, 0, hello_req, sizeof hello_req);
    assert_tx(1, 0, 1, expected, len);
}

/* covers: SES-3, HB-1 */
void test_session_announcing_answers_other_frames_with_a_rate_limited_hello(void)
{
    const uint8_t user[] = {0x00u};
    const uint8_t ping[] = {0x01u, 0x2au};
    uint8_t expected[64];
    uint16_t len;

    configure(one_topic, 1, 0x44332211u);
    len = hello_payload(expected, 0x44332211u, 1);

    feed_host(16, 0, user, sizeof user);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(1, 0, 1, expected, len);

    /* Three more at once: the 100 ms rate limit holds the reply. */
    feed_host(16, 1, user, sizeof user);
    feed_host(1, 0, ping, sizeof ping);
    feed_host(4, 0, NULL, 0);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());

    fake_port_advance(100);
    feed_host(1, 1, ping, sizeof ping);
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    assert_tx(2, 0, 2, expected, len);
    TEST_ASSERT_EQUAL_UINT16(5, r2s_framer_counters()->rx_frames);
    TEST_ASSERT_EQUAL_UINT16(0, r2s_framer_counters()->dropped_frames);
}

/* covers: SYS-4 */
void test_session_a_reserved_channel_is_ignored_without_a_reply_or_a_count(void)
{
    const r2s_framer_counters_t *count;

    configure(one_topic, 1, 0x44332211u);
    /* The shape of spec/vectors/frames/reserved_channel.yaml. */
    feed_host(9, 0, NULL, 0);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    count = r2s_framer_counters();
    TEST_ASSERT_EQUAL_UINT16(1, count->rx_frames);
    TEST_ASSERT_EQUAL_UINT16(0, count->dropped_frames);
}

/* covers: SES-5 */
void test_session_announcing_repeats_hello_every_second_without_host_traffic(void)
{
    uint8_t expected[64];
    uint16_t len;

    configure(one_topic, 1, 0x44332211u);
    len = hello_payload(expected, 0x44332211u, 1);

    fake_port_advance(999);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());

    fake_port_advance(1);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(1, 0, 1, expected, len);
}

/* covers: SYS-5, CTL-14, CTL-16 */
void test_session_malformed_control_frames_are_refused(void)
{
    const uint8_t ready_extra[] = {0x06u, 0x00u};
    const uint8_t topic_req_far[] = {0x03u, 0x05u};
    const uint8_t unknown_opcode[] = {0x09u};
    const uint8_t hello_req_long[] = {0x01u, 0x01u, 0x02u};
    const uint8_t heartbeat_full[] = {0x00u};
    const uint8_t status_bad[] = {0x05u, 0x10u, 0x07u};
    const uint8_t status_unknown[] = {0x05u, 0x12u, 0x00u};

    configure(one_topic, 1, 0x44332211u);
    feed_host(0, 0, ready_extra, sizeof ready_extra);
    feed_host(0, 1, topic_req_far, sizeof topic_req_far);
    feed_host(0, 2, NULL, 0);
    feed_host(0, 3, unknown_opcode, sizeof unknown_opcode);
    feed_host(0, 4, hello_req_long, sizeof hello_req_long);
    feed_host(4, 0, heartbeat_full, sizeof heartbeat_full);
    feed_host(0, 5, status_bad, sizeof status_bad);
    feed_host(0, 6, status_unknown, sizeof status_unknown);

    TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    TEST_ASSERT_EQUAL_UINT16(8, r2s_framer_counters()->dropped_frames);
}

/* covers: FRM-4, FRM-5, SES-11 */
void test_session_sequence_numbers_increment_and_restart_after_each_handshake(void)
{
    const uint8_t hello_req[] = {0x01u, 0x01u};
    const uint8_t topic_req[] = {0x03u, 0x00u};
    const uint8_t status[] = {0x05u, 0x10u, 0x00u};
    const uint8_t ready[] = {0x06u};
    const uint8_t heartbeat[] = {0x04u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    const uint8_t heartbeat_next[] = {0x06u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    uint8_t expected[64];
    uint16_t len;

    configure(one_topic, 1, 0x44332211u);
    len = hello_payload(expected, 0x44332211u, 1);
    assert_tx(0, 0, 0, expected, len);

    feed_host(0, 0, hello_req, sizeof hello_req);
    assert_tx(1, 0, 1, expected, len);

    feed_host(0, 1, topic_req, sizeof topic_req);
    assert_tx(2, 0, 2, topic_frame_payload, sizeof topic_frame_payload);

    feed_host(0, 2, status, sizeof status);
    feed_host(0, 3, ready, sizeof ready);

    fake_port_advance(500);
    r2s_spin_some();
    assert_tx(3, 4, 0, heartbeat, sizeof heartbeat);
    fake_port_advance(500);
    r2s_spin_some();
    assert_tx(4, 4, 1, heartbeat, sizeof heartbeat);

    /* The first READY already reset the counters, so the re-announce answers
     * from 0; the second READY resets them again. */
    fake_port_advance(2000);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
    feed_host(0, 4, hello_req, sizeof hello_req);
    assert_tx(5, 0, 0, expected, len);
    feed_host(0, 5, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    fake_port_advance(500);
    r2s_spin_some();
    assert_tx(6, 4, 0, heartbeat_next, sizeof heartbeat_next);
}

/* READY is the only way out of ANNOUNCING; the publish gate reads
 * r2s_connected() once the codec lands it. */
void test_session_announcing_holds_until_ready(void)
{
    const uint8_t topic_req[] = {0x03u, 0x00u};

    configure(one_topic, 1, 0x44332211u);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
    feed_host(0, 0, topic_req, sizeof topic_req);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
}

/* A sequence gap is accepted, not dropped, and leaves the link alone. */
void test_session_user_frames_with_a_sequence_gap_do_not_disturb_the_link(void)
{
    const uint8_t hello_req[] = {0x01u, 0x01u};
    const uint8_t status[] = {0x05u, 0x10u, 0x00u};
    const uint8_t ready[] = {0x06u};
    const uint8_t user[] = {0x00u};
    const r2s_framer_counters_t *count;

    configure(one_topic, 1, 0x44332211u);
    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, status, sizeof status);
    feed_host(0, 2, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());

    /* The gap is accepted, not dropped: gap counters have no reporting
     * channel in this spec version, so none is kept. */
    feed_host(16, 0, user, sizeof user);
    feed_host(16, 1, user, sizeof user);
    feed_host(16, 5, user, sizeof user);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    count = r2s_framer_counters();
    TEST_ASSERT_EQUAL_UINT16(6, count->rx_frames);
    TEST_ASSERT_EQUAL_UINT16(0, count->dropped_frames);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
}

/* covers: HB-2, HB-3 */
void test_session_the_heartbeat_carries_every_counter(void)
{
    const uint8_t hello_req[] = {0x01u, 0x01u};
    const uint8_t ready[] = {0x06u};
    const uint8_t bad[] = {0x01u, 0x02u, 0x03u};
    const uint8_t heartbeat[] = {0x02u, 0x00u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u};
    uint16_t len;

    configure(one_topic, 1, 0x44332211u);
    len = r2s_test_frame_bad_crc(wire, 16, 0, bad, sizeof bad);
    feed_slow(wire, len);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());

    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, ready, sizeof ready);
    fake_port_advance(500);
    r2s_spin_some();
    /* Two frames accepted, one dropped for its CRC and counted twice. */
    assert_tx(2, 4, 0, heartbeat, sizeof heartbeat);
}

/* covers: CTL-15, FRM-3 */
void test_session_a_spin_answers_every_queued_frame_and_ignores_user_topics(void)
{
    const uint8_t hello_req[] = {0x01u, 0x01u};
    const uint8_t topic_req[] = {0x03u, 0x00u};
    const uint8_t status[] = {0x05u, 0x10u, 0x00u};
    const uint8_t ready[] = {0x06u};
    const uint8_t user[] = {0x00u};
    uint8_t expected[64];
    uint16_t len;
    uint16_t first;
    uint16_t second;

    configure(one_topic, 1, 0x44332211u);
    len = hello_payload(expected, 0x44332211u, 1);

    first = r2s_test_frame(wire, 0, 0, hello_req, sizeof hello_req);
    fake_port_queue_rx(wire, first);
    second = r2s_test_frame(wire, 0, 1, topic_req, sizeof topic_req);
    fake_port_queue_rx(wire, second);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    assert_tx(1, 0, 1, expected, len);
    assert_tx(2, 0, 2, topic_frame_payload, sizeof topic_frame_payload);

    feed_host(0, 2, status, sizeof status);
    feed_host(0, 3, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());

    /* A user topic has no consumer yet; it is accepted and ignored. */
    feed_host(16, 0, user, sizeof user);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
}

/* A configuration the handshake cannot announce fails precisely. */
void test_session_an_unannounceable_configuration_is_refused(void)
{
    static char far_name[65537];
    r2s_session_topic_t far[17];
    uint8_t topic_count = 17;

    memset(far_name, 'a', sizeof far_name);
    far_name[sizeof far_name - 1u] = '\0';
    for (uint8_t i = 0; i < topic_count; i++) {
        far[i] = one_topic[0];
    }
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_session_configure("node", far, topic_count));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_session_configure(NULL, one_topic, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_session_configure("node", NULL, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_session_configure(far_name, one_topic, 1));

    /* The refusals changed nothing: a valid table still configures. */
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
}

void test_session_without_a_configuration_announces_no_name_and_no_topics(void)
{
    uint8_t expected[64];
    uint16_t len;

    fake_port_set_boot_id(0x44332211u);
    r2s_init();
    TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    len = hello_payload(expected, 0x44332211u, 0);
    expected[17] = 0x00u;
    assert_tx(0, 0, 0, expected, (uint16_t)(len - 4u));
}

/* A configuration without topics is valid: the node announces its name and
 * an empty table, and a null table with topics is not. */
void test_session_a_configuration_without_topics_is_accepted(void)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", NULL, 0u));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_session_configure("node", NULL, 1u));
}

/* A name that fits the varint but not the hello frame is refused. */
void test_session_a_name_that_does_not_fit_the_hello_frame_is_refused(void)
{
    static char long_name[601];

    memset(long_name, 'b', sizeof long_name - 1u);
    long_name[sizeof long_name - 1u] = '\0';
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_session_configure(long_name, one_topic, 1));
}

/* A topic without names, or one whose announcement does not fit a frame, is
 * refused. */
void test_session_a_nameless_or_oversized_topic_is_refused(void)
{
    static const r2s_session_topic_t no_type[] = {
        {0x10u, 0x03u, {0u}, NULL, "float"},
    };
    static const r2s_session_topic_t no_name[] = {
        {0x10u, 0x03u, {0u}, "std_msgs/msg/Float32", NULL},
    };
    static char big_type[301];
    static char big_topic[301];
    static r2s_session_topic_t big[1];

    memset(big_type, 't', sizeof big_type - 1u);
    big_type[sizeof big_type - 1u] = '\0';
    memset(big_topic, 'n', sizeof big_topic - 1u);
    big_topic[sizeof big_topic - 1u] = '\0';
    big[0] = one_topic[0];
    big[0].type_name = big_type;
    big[0].topic_name = big_topic;
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_session_configure("node", no_type, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_session_configure("node", no_name, 1));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_session_configure("node", big, 1));
}

/* A frame that finds no space is not started: the boot hello, a topic and
 * the heartbeat all wait for room instead of sending half a frame. */
void test_session_frames_wait_for_space_instead_of_sending_half(void)
{
    const uint8_t topic_req[] = {0x03u, 0x00u};
    const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
    const uint8_t ready[] = {0x06u};

    configure(one_topic, 1, 0x44332211u);
    fake_port_tx_clear();
    fake_port_set_tx_free(9u);
    r2s_init();
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, topic_req, sizeof topic_req);
    fake_port_tx_clear();
    fake_port_set_tx_free(9u);
    feed_host(0, 2, topic_req, sizeof topic_req);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
    feed_host(0, 3, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    fake_port_tx_clear();
    fake_port_set_tx_free(9u);
    fake_port_advance(500);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
}

/* A topic request past the table, and a status the table does not know, are
 * refused without a reply. */
void test_session_unknown_topic_requests_are_refused(void)
{
    const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
    const uint8_t bad_req[] = {0x03u, 0x05u};
    const uint8_t bad_status[] = {0x05u, 0x10u, 0x09u};
    const uint16_t drops_before = r2s_framer_counters()->dropped_frames;

    configure(one_topic, 1, 0x44332211u);
    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, bad_req, sizeof bad_req);
    feed_host(0, 2, bad_status, sizeof bad_status);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(drops_before + 2u), r2s_framer_counters()->dropped_frames);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
}

/* Once connected, a repeated ready, a heartbeat and a later channel's frame
 * are accepted and ignored: the link is already up. */
void test_session_connected_ignores_redundant_and_later_frames(void)
{
    const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
    const uint8_t ping[] = {0x01u, 0x2au};
    const uint8_t ready[] = {0x06u};

    configure(one_topic, 1, 0x44332211u);
    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    fake_port_tx_clear();
    feed_host(0, 2, ready, sizeof ready);
    feed_host(4, 0, NULL, 0);
    feed_host(1, 0, ping, sizeof ping);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* A spin serves at most R2S_SPIN_MAX_FRAMES frames, leaving the rest queued
 * for the next one. */
void test_session_a_spin_serves_at_most_eight_frames(void)
{
    static uint8_t queued[WIRE_MAX];
    const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
    const uint8_t ready[] = {0x06u};
    uint16_t before;
    uint8_t user[] = {0x00u};
    uint16_t i;

    configure(one_topic, 1, 0x44332211u);
    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, ready, sizeof ready);
    before = r2s_framer_counters()->rx_frames;
    for (i = 0u; i < 10u; i++) {
        uint16_t wire_len = r2s_test_frame(queued, 16, (uint8_t)i, user, sizeof user);

        fake_port_queue_rx(queued, wire_len);
    }
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(before + 8u), r2s_framer_counters()->rx_frames);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(before + 10u), r2s_framer_counters()->rx_frames);
}
