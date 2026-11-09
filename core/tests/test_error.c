// Copyright (c) 2026 Alexis Pojomovsky
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

/* The MCU error report channel through the fake port: what the core sends on
 * topic ID 5 for each error code, and the rate limits that hold reports back
 * (spec/protocol.md, section 7.5). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "handwritten.h"
#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_error.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"
#include "unity.h"

/* The encoded form of a decoded frame one byte past the limit, plus its
 * delimiter, with room to spare past the worst case the bound allows. */
#define WIRE_BIG (R2S_MAX_FRAME + R2S_MAX_FRAME / 254 + 8u)

static uint8_t wire[WIRE_BIG];

void r2s_on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
    (void)msg;
}

void r2s_on_cmd_str(const std_msgs__msg__String *msg)
{
    (void)msg;
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

/* The full handshake for the four handwritten topics: every request names
 * its topic, and every status accepts it. */
static void handshake(void)
{
    const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
    const uint8_t topic_req0[] = {0x03u, 0x00u};
    const uint8_t topic_req1[] = {0x03u, 0x01u};
    const uint8_t topic_req2[] = {0x03u, 0x02u};
    const uint8_t topic_req3[] = {0x03u, 0x03u};
    const uint8_t status_battery[] = {0x05u, R2S_HANDWRITTEN_BATTERY_ID, 0x00u};
    const uint8_t status_cmd_vel[] = {0x05u, R2S_HANDWRITTEN_CMD_VEL_ID, 0x00u};
    const uint8_t status_log_text[] = {0x05u, R2S_HANDWRITTEN_LOG_TEXT_ID, 0x00u};
    const uint8_t status_cmd_str[] = {0x05u, R2S_HANDWRITTEN_CMD_STR_ID, 0x00u};
    const uint8_t ready[] = {0x06u, 0x01u, 0x00u, 0x00u, 0x00u};

    feed_host(0, 0, hello_req, sizeof hello_req);
    feed_host(0, 1, topic_req0, sizeof topic_req0);
    feed_host(0, 2, topic_req1, sizeof topic_req1);
    feed_host(0, 3, topic_req2, sizeof topic_req2);
    feed_host(0, 4, topic_req3, sizeof topic_req3);
    feed_host(0, 5, status_battery, sizeof status_battery);
    feed_host(0, 6, status_cmd_vel, sizeof status_cmd_vel);
    feed_host(0, 7, status_log_text, sizeof status_log_text);
    feed_host(0, 8, status_cmd_str, sizeof status_cmd_str);
    feed_host(0, 9, ready, sizeof ready);
    TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
}

static void configure(void)
{
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_handwritten_configure());
    r2s_init();
}

/* Checks that the index-th captured frame carries the error payload on topic
 * 5 with the report sequence the test names. */
static void assert_error_report(uint16_t index, uint8_t seq, const uint8_t *payload)
{
    static uint8_t expected[WIRE_BIG];
    char label[64];
    uint16_t encoded_len;
    const uint8_t *encoded;
    uint16_t expected_len = r2s_test_frame(expected, 5u, seq, payload, 5u);

    snprintf(label, sizeof label, "error report %u", index);
    encoded = fake_port_tx_frame(index, &encoded_len);
    TEST_ASSERT_NOT_NULL_MESSAGE(encoded, label);
    R2S_ASSERT_BYTES(expected, (uint16_t)(expected_len - 1u), encoded, encoded_len, label);
}

/* covers: ERR-1, ERR-2, ERR-3, ERR-6 */
void test_error_not_connected_publish_reports_once_per_connection(void)
{
    std_msgs__msg__Float32 battery = {1.5f};
    /* code 5, battery, one occurrence, no field. */
    const uint8_t report[] = {0x05u, R2S_HANDWRITTEN_BATTERY_ID, 0x01u, 0x00u, 0xFFu};

    configure();
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_error_report(1, 0, report);
    /* A second failure while still announcing counts without sending again. */
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
}

/* covers: ERR-3, ERR-6 */
void test_error_announce_failures_flush_on_connect(void)
{
    std_msgs__msg__Float32 battery = {1.5f};
    /* code 5, battery, the failure the first report did not carry, no field. */
    const uint8_t report[] = {0x05u, R2S_HANDWRITTEN_BATTERY_ID, 0x01u, 0x00u, 0xFFu};

    configure();
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, r2s_publish_battery(&battery));
    fake_port_tx_clear();
    handshake();
    /* The handshake answers with a HELLO and four TOPIC frames before the
     * pending failure flushes on READY, with the report sequence restarted
     * by the handshake. */
    TEST_ASSERT_EQUAL_UINT16(6, fake_port_tx_frame_count());
    assert_error_report(5, 0, report);
}

/* covers: ERR-3, ERR-6 */
void test_error_not_connected_count_wraps_past_65535(void)
{
    /* code 5, battery, 40000 occurrences, no field. */
    const uint8_t first[] = {0x05u, R2S_HANDWRITTEN_BATTERY_ID, 0x40u, 0x9Cu, 0xFFu};
    /* 30000 + 40000 more while the next report waits: 70000 past the wrap
     * reads back as 4464 in the next connection. */
    const uint8_t wrapped[] = {0x05u, R2S_HANDWRITTEN_BATTERY_ID, 0x70u, 0x11u, 0xFFu};

    configure();
    handshake();
    fake_port_tx_clear();
    r2s_error_note(R2S_ERROR_NOT_CONNECTED, R2S_HANDWRITTEN_BATTERY_ID, 0xFFu, 40000u);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_error_report(0, 0, first);
    r2s_error_note(R2S_ERROR_NOT_CONNECTED, R2S_HANDWRITTEN_BATTERY_ID, 0xFFu, 30000u);
    r2s_error_note(R2S_ERROR_NOT_CONNECTED, R2S_HANDWRITTEN_BATTERY_ID, 0xFFu, 40000u);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    handshake();
    /* The handshake answers with a HELLO and four TOPIC frames before the
     * wrapped count flushes on READY, with the report sequence restarted
     * by the handshake. */
    TEST_ASSERT_EQUAL_UINT16(7, fake_port_tx_frame_count());
    assert_error_report(6, 0, wrapped);
}

/* covers: ERR-2, ERR-3, ERR-6 */
void test_error_busy_publish_reports_at_most_once_per_second(void)
{
    std_msgs__msg__Float32 battery = {1.5f};
    /* code 6, battery, two occurrences since the first report, no field. */
    const uint8_t second[] = {0x06u, R2S_HANDWRITTEN_BATTERY_ID, 0x02u, 0x00u, 0xFFu};

    configure();
    handshake();
    /* A battery frame of 4 payload bytes takes 10 wire bytes; the cap fits
     * it and nothing else, so the report never fits while capped. */
    fake_port_set_tx_free(10u);
    fake_port_tx_clear();
    TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    /* A second busy publish at the same tick counts without sending again. */
    TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY, r2s_publish_battery(&battery));
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    /* A second of link time releases the two pending failures, ahead of the
     * heartbeat the same second releases. */
    fake_port_set_tx_free(0xFFFFu);
    fake_port_tx_clear();
    fake_port_advance(1000u);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    assert_error_report(0, 0, second);
}

/* covers: ERR-1, ERR-2, ERR-6 */
void test_error_unknown_topic_id_reports_code_2(void)
{
    /* code 2, the unknown id, one occurrence, no field. 0x13 is cmd_str
     * since the fourth topic landed, so the probe uses 0x14. */
    const uint8_t report[] = {0x02u, 0x14u, 0x01u, 0x00u, 0xFFu};
    const uint8_t payload[] = {0x00u};

    configure();
    handshake();
    fake_port_tx_clear();
    feed_host(0x14u, 0, payload, sizeof payload);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_error_report(0, 0, report);
}

/* covers: ERR-2 */
void test_error_known_but_unsubscribed_topic_reports_nothing(void)
{
    const uint8_t payload[] = {0x00u, 0x00u, 0xC0u, 0x3Fu};
    uint16_t dropped_before = r2s_framer_counters()->dropped_frames;

    configure();
    handshake();
    fake_port_tx_clear();
    /* Battery is announced but published by the MCU: a host frame on it is
     * refused without an error report, since its id is known. */
    feed_host(R2S_HANDWRITTEN_BATTERY_ID, 0, payload, sizeof payload);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(dropped_before + 1u), r2s_framer_counters()->dropped_frames);
}

/* covers: ERR-2, ERR-6 */
void test_error_receive_overflow_reports_code_3(void)
{
    /* code 3, no topic, one occurrence, no field. */
    const uint8_t report[] = {0x03u, 0xFFu, 0x01u, 0x00u, 0xFFu};
    /* More nonzero bytes than the framer holds, then the delimiter that
     * drops them: the buffer fills while bytes are still coming. */
    static uint8_t flood[R2S_FRAMER_RX_CAPACITY + 8u];
    uint16_t i;

    for (i = 0; i < sizeof flood; i++) {
        flood[i] = 0x11u;
    }
    flood[sizeof flood - 1u] = 0x00u;
    configure();
    handshake();
    fake_port_tx_clear();
    feed_slow(flood, sizeof flood);
    assert_error_report(0, 0, report);
}

/* covers: ERR-2, ERR-6 */
void test_error_frame_too_large_reports_code_4_with_its_topic(void)
{
    /* code 4, battery, one occurrence, no field. */
    const uint8_t report[] = {0x04u, R2S_HANDWRITTEN_BATTERY_ID, 0x01u, 0x00u, 0xFFu};
    /* A decoded frame one byte past the limit the build accepts. Zeros keep
     * the encoded form inside the receive buffer, so the frame decodes and
     * fails the size check instead of overflowing the buffer first. */
    static uint8_t big[R2S_MAX_FRAME];
    uint16_t wire_len;

    memset(big, 0x00, sizeof big);
    configure();
    handshake();
    fake_port_tx_clear();
    wire_len = r2s_test_frame(wire, R2S_HANDWRITTEN_BATTERY_ID, 0, big, (uint16_t)(R2S_MAX_FRAME - 3u));
    feed_slow(wire, wire_len);
    assert_error_report(0, 0, report);
}

/* covers: ERR-2 */
void test_error_note_ignores_a_code_outside_1_to_6(void)
{
    configure();
    handshake();
    fake_port_tx_clear();
    r2s_error_note(0u, R2S_HANDWRITTEN_BATTERY_ID, 0xFFu, 1u);
    r2s_error_note(7u, R2S_HANDWRITTEN_BATTERY_ID, 0xFFu, 1u);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: ERR-2, ERR-6 */
void test_error_report_waits_for_transmitter_space(void)
{
    /* code 2, the unknown id, one occurrence, no field. */
    const uint8_t report[] = {0x02u, 0x14u, 0x01u, 0x00u, 0xFFu};
    const uint8_t payload[] = {0x00u};

    configure();
    handshake();
    /* A cap below the report's wire bound refuses the send: the count
     * stays pending instead of a half-sent frame going out. */
    fake_port_set_tx_free(10u);
    fake_port_tx_clear();
    feed_host(0x14u, 0, payload, sizeof payload);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    fake_port_set_tx_free(0xFFFFu);
    r2s_spin_some();
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_error_report(0, 0, report);
}

/* covers: ERR-2 */
void test_error_status_for_an_unknown_topic_reports_code_2(void)
{
    /* code 2, the unknown id, one occurrence, no field. */
    const uint8_t report[] = {0x02u, 0x42u, 0x01u, 0x00u, 0xFFu};
    const uint8_t status[] = {0x05u, 0x42u, 0x00u};

    configure();
    handshake();
    fake_port_tx_clear();
    feed_host(0, 9, status, sizeof status);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_error_report(0, 0, report);
}

/* covers: ERR-1, ERR-2, ERR-4, ERR-6 */
void test_error_bound_violation_reports_code_1_with_its_field(void)
{
    /* code 1, cmd_str, one occurrence, field 0 names the string itself. */
    const uint8_t report[] = {0x01u, R2S_HANDWRITTEN_CMD_STR_ID, 0x01u, 0x00u, 0x00u};
    /* A string of nine bytes over the bound of 8. */
    const uint8_t payload[] = {0x09u, 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i'};

    configure();
    handshake();
    fake_port_tx_clear();
    feed_host(R2S_HANDWRITTEN_CMD_STR_ID, 0, payload, sizeof payload);
    TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    assert_error_report(0, 0, report);
}

/* covers: ERR-2 */
void test_error_malformed_payload_reports_nothing(void)
{
    const uint8_t payload[] = {0x00u};
    uint16_t dropped_before;

    configure();
    handshake();
    /* A truncated Twist breaks an encoding rule rather than a bound, so the
     * dispatch drops it without an error report. */
    dropped_before = r2s_framer_counters()->dropped_frames;
    fake_port_tx_clear();
    feed_host(R2S_HANDWRITTEN_CMD_VEL_ID, 0, payload, sizeof payload);
    TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(dropped_before + 1u), r2s_framer_counters()->dropped_frames);
}
