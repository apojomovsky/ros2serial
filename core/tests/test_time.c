/* The MCU side of time sync through the fake port: PING answers carry the
 * delimiter tick, CORRECTIONs anchor r2s_now(), and bad ones are refused. */
#include <float.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_time.h"
#include "r2s/r2s_varint.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

static uint8_t wire[WIRE_MAX];

static const r2s_topic_t one_topic[] = {
    {0x10u, 0x03u, {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u}, "std_msgs/msg/Float32", "float"},
};

#if DBL_MANT_DIG == FLT_MANT_DIG
#define EXPECTED_FEATURES 0x13u
#else
#define EXPECTED_FEATURES 0x03u
#endif

/* Queues bytes one at a time, spinning after each, as a slow link delivers
 * them. */
static void feed_slow(const uint8_t *data, uint16_t len)
{
    uint16_t i;

    for (i = 0u; i < len; i++) {
        fake_port_queue_rx(&data[i], 1);
        r2s_spin_some();
    }
}

/* Builds a host frame and feeds it byte by byte. The session never checks a
 * host seq on the system channels, so every test uses 0. */
static void feed_host(uint8_t topic_id, const uint8_t *payload, uint16_t len)
{
    uint16_t wire_len = r2s_test_frame(wire, topic_id, 0u, payload, len);

    feed_slow(wire, wire_len);
}

/* The handshake the build announces, up to READY. */
static void connect(void)
{
    static const uint8_t hello_req[] = {0x01u, 0x01u};
    static const uint8_t topic_req[] = {0x03u, 0x00u};
    static const uint8_t topic_status[] = {0x05u, 0x10u, 0x00u};
    static const uint8_t ready[] = {0x06u, 0x01u, 0x00u, 0x00u, 0x00u};

    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
    r2s_init();
    feed_host(0u, hello_req, sizeof hello_req);
    feed_host(0u, topic_req, sizeof topic_req);
    feed_host(0u, topic_status, sizeof topic_status);
    feed_host(0u, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
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

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8u) & 0xFFu);
    out[2] = (uint8_t)((value >> 16u) & 0xFFu);
    out[3] = (uint8_t)((value >> 24u) & 0xFFu);
}

/* Feeds a CORRECTION for the anchor it carries. */
static void send_correction(uint32_t tick0, int32_t sec0, uint32_t nanosec0, int16_t drift_ppm)
{
    uint8_t payload[15];

    payload[0] = 0x03u;
    put_le32(payload + 1u, tick0);
    put_le32(payload + 5u, (uint32_t)sec0);
    put_le32(payload + 9u, nanosec0);
    payload[13] = (uint8_t)((uint16_t)drift_ppm & 0xFFu);
    payload[14] = (uint8_t)(((uint16_t)drift_ppm >> 8u) & 0xFFu);
    feed_host(1u, payload, sizeof payload);
}

/* covers: TIM-1, TIM-2 */
void test_time_pong_carries_ping_id_and_delimiter_tick(void)
{
    /* The tick moves between the first byte and the delimiter, so the PONG
     * proves which arrival it stamps. */
    static const uint8_t ping[] = {0x01u, 0x7Eu};
    uint8_t expected[6];
    uint16_t wire_len;

    fake_port_set_tick(50000u);
    connect();
    fake_port_tx_clear();

    wire_len = r2s_test_frame(wire, 1u, 0u, ping, sizeof ping);
    fake_port_set_tick(50100u);
    feed_slow(wire, (uint16_t)(wire_len - 1u));
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tick(50200u);
    feed_slow(wire + wire_len - 1u, 1u);

    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    expected[0] = 0x02u;
    expected[1] = 0x7Eu;
    put_le32(expected + 2u, 50200u);
    assert_tx(0, 1u, 0u, expected, sizeof expected);

    /* The next PONG advances the time channel's own seq. */
    feed_host(1u, ping, sizeof ping);
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_tx(1, 1u, 1u, expected, sizeof expected);
}

/* covers: TIM-1, TIM-2 */
void test_time_pong_busy_port_sends_nothing(void)
{
    /* A PONG is best effort: without room for its whole frame the session
     * drops it instead of sending half a frame. */
    static const uint8_t ping[] = {0x01u, 0x2au};

    fake_port_set_tick(50000u);
    connect();
    fake_port_tx_clear();
    fake_port_set_tx_free(11u);
    feed_host(1u, ping, sizeof ping);

    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
}

/* covers: TIM-3, TIM-6 */
void test_time_correction_anchors_now(void)
{
    r2s_time_t got;

    fake_port_set_tick(50000u);
    connect();
    send_correction(50000u, 100, 0u, 0);

    fake_port_set_tick(50250u);
    got = r2s_now();
    TEST_ASSERT_EQUAL_UINT8(1, r2s_time_synced());
    TEST_ASSERT_EQUAL_INT32(100, got.sec);
    TEST_ASSERT_EQUAL_UINT32(250000000u, got.nanosec);
}

/* covers: TIM-6 */
void test_time_tick_wraparound_is_an_unsigned_difference(void)
{
    r2s_time_t got;

    fake_port_set_tick(0xFFFFFF00u);
    connect();
    send_correction(0xFFFFFF00u, 10, 0u, 0);

    fake_port_set_tick(0x100u);
    got = r2s_now();
    TEST_ASSERT_EQUAL_UINT8(1, r2s_time_synced());
    TEST_ASSERT_EQUAL_INT32(10, got.sec);
    TEST_ASSERT_EQUAL_UINT32(512000000u, got.nanosec);
}

/* covers: TIM-4, TIM-6 */
void test_time_drift_applies_per_elapsed_millisecond(void)
{
    r2s_time_t got;

    fake_port_set_tick(80000u);
    connect();
    send_correction(80000u, 100, 0u, 20000);

    fake_port_set_tick(81000u);
    got = r2s_now();
    TEST_ASSERT_EQUAL_INT32(101, got.sec);
    TEST_ASSERT_EQUAL_UINT32(20000000u, got.nanosec);

    send_correction(80000u, 5, 0u, -20000);
    fake_port_set_tick(81000u);
    got = r2s_now();
    TEST_ASSERT_EQUAL_INT32(5, got.sec);
    TEST_ASSERT_EQUAL_UINT32(980000000u, got.nanosec);
}

/* covers: TIM-4, TIM-5, TIM-11 */
void test_time_compute_refuses_inputs_the_wire_refuses(void)
{
    /* A sentinel: a refusal leaves *out untouched. */
    r2s_time_t out = {7, 8};

    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_compute(999u, 0u, 0, 0u, 0, 0u, &out));
    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_compute(1500u, 0u, 0, 0u, 0, 0u, &out));
    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_compute(3000u, 0u, 0, 0u, 0, 0u, &out));
    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_compute(1000u, 0u, 0, 0u, 20001, 0u, &out));
    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_compute(1000u, 0u, 0, 0u, -20001, 0u, &out));
    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_compute(1000u, 0u, 0, 1000000000u, 0, 0u, &out));
    TEST_ASSERT_EQUAL_INT32(7, out.sec);
    TEST_ASSERT_EQUAL_UINT32(8u, out.nanosec);
}

/* covers: TIM-9 */
void test_time_nanoseconds_normalize_before_and_after_the_drift(void)
{
    r2s_time_t got;

    fake_port_set_tick(80000u);
    connect();
    send_correction(80000u, 100, 999999999u, 20000);

    fake_port_set_tick(80001u);
    got = r2s_now();
    TEST_ASSERT_EQUAL_INT32(101, got.sec);
    TEST_ASSERT_EQUAL_UINT32(1019999u, got.nanosec);
}

/* covers: TIM-7 */
void test_time_now_is_unsynced_without_a_correction(void)
{
    r2s_time_t got;

    fake_port_set_tick(50000u);
    connect();

    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_synced());
    got = r2s_now();
    TEST_ASSERT_EQUAL_INT32(0, got.sec);
    TEST_ASSERT_EQUAL_UINT32(0u, got.nanosec);
}

/* covers: TIM-9 */
void test_time_drift_can_carry_into_the_next_second(void)
{
    r2s_time_t got;

    fake_port_set_tick(80000u);
    connect();
    send_correction(80000u, 100, 999999999u, 20000);

    fake_port_set_tick(80999u);
    got = r2s_now();
    TEST_ASSERT_EQUAL_INT32(102, got.sec);
    TEST_ASSERT_EQUAL_UINT32(18979999u, got.nanosec);
}

void test_time_anchor_older_than_the_maximum_is_unsynced(void)
{
    r2s_time_t got;

    fake_port_set_tick(50000u);
    connect();
    send_correction(50000u, 100, 0u, 0);

    fake_port_set_tick(60000u);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_time_synced());
    got = r2s_now();
    TEST_ASSERT_EQUAL_INT32(110, got.sec);
    TEST_ASSERT_EQUAL_UINT32(0u, got.nanosec);

    fake_port_set_tick(60001u);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_synced());
    got = r2s_now();
    TEST_ASSERT_EQUAL_INT32(0, got.sec);
    TEST_ASSERT_EQUAL_UINT32(0u, got.nanosec);
}

/* covers: TIM-5 */
void test_time_correction_outside_the_drift_limit_is_refused(void)
{
    r2s_time_t got;

    fake_port_set_tick(50000u);
    connect();
    send_correction(50000u, 100, 0u, 0);
    send_correction(50000u, 200, 0u, 20001);
    send_correction(50000u, 200, 0u, -20001);

    /* The refused corrections change nothing: the old anchor still answers. */
    fake_port_set_tick(51000u);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_time_synced());
    got = r2s_now();
    TEST_ASSERT_EQUAL_INT32(101, got.sec);
    TEST_ASSERT_EQUAL_UINT32(0u, got.nanosec);
    TEST_ASSERT_EQUAL_UINT16(2, r2s_framer_counters()->dropped_frames);
}

/* covers: TIM-11 */
void test_time_correction_with_a_full_second_of_nanosec_is_refused(void)
{
    fake_port_set_tick(50000u);
    connect();
    send_correction(50000u, 100, 1000000000u, 0);

    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_synced());
    TEST_ASSERT_EQUAL_UINT16(1, r2s_framer_counters()->dropped_frames);
}

/* covers: TIM-10 */
void test_time_malformed_time_frames_are_refused(void)
{
    static const uint8_t short_ping[] = {0x01u};
    static const uint8_t long_ping[] = {0x01u, 0x00u, 0x00u};
    static const uint8_t unknown_opcode[] = {0x09u};
    static const uint8_t short_correction[] = {0x03u, 0x00u, 0x00u};

    fake_port_set_tick(50000u);
    connect();
    feed_host(1u, short_ping, sizeof short_ping);
    feed_host(1u, long_ping, sizeof long_ping);
    feed_host(1u, unknown_opcode, sizeof unknown_opcode);
    feed_host(1u, short_correction, sizeof short_correction);

    /* The handshake's three frames, and nothing the refused frames answer. */
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    TEST_ASSERT_EQUAL_UINT16(4, r2s_framer_counters()->dropped_frames);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_synced());
}

/* covers: TIM-10 */
void test_time_empty_time_frame_is_refused(void)
{
    fake_port_set_tick(50000u);
    connect();
    feed_host(1u, NULL, 0);

    /* The handshake's three frames, and nothing the refused frame answers. */
    TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    TEST_ASSERT_EQUAL_UINT16(1, r2s_framer_counters()->dropped_frames);
    TEST_ASSERT_EQUAL_UINT8(0, r2s_time_synced());
}

/* covers: SES-3 */
void test_time_ping_while_announcing_answers_hello(void)
{
    static const uint8_t ping[] = {0x01u, 0x7Eu};
    uint8_t expected[64];
    uint16_t len = 0u;
    uint16_t name_len = 4u;

    fake_port_set_tick(50000u);
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
    r2s_init();
    feed_host(1u, ping, sizeof ping);

    /* The boot HELLO and the rate-limited answer, and no PONG. */
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    expected[len++] = 0x02u;
    expected[len++] = R2S_PROTOCOL_VERSION;
    expected[len++] = 0x00u;
    expected[len++] = 0x00u;
    expected[len++] = 0x00u;
    expected[len++] = 0x00u;
    expected[len++] = 0x00u;
    expected[len++] = (uint8_t)(R2S_MAX_FRAME & 0xFFu);
    expected[len++] = (uint8_t)((R2S_MAX_FRAME >> 8u) & 0xFFu);
    expected[len++] = (uint8_t)(R2S_FRAMER_RX_CAPACITY & 0xFFu);
    expected[len++] = (uint8_t)((R2S_FRAMER_RX_CAPACITY >> 8u) & 0xFFu);
    expected[len++] = (uint8_t)(R2S_TICK_HZ & 0xFFu);
    expected[len++] = (uint8_t)((R2S_TICK_HZ >> 8u) & 0xFFu);
    expected[len++] = (uint8_t)((R2S_TICK_HZ >> 16u) & 0xFFu);
    expected[len++] = (uint8_t)((R2S_TICK_HZ >> 24u) & 0xFFu);
    expected[len++] = EXPECTED_FEATURES;
    expected[len++] = 0x01u;
    len += r2s_varint_encode(name_len, expected + len);
    memcpy(expected + len, "node", name_len);
    len = (uint16_t)(len + name_len);
    assert_tx(1, 0u, 1u, expected, len);
}
