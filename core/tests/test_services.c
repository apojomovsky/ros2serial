/* The MCU side of the services channel: the table it announces, the walk it
 * answers, and the REQUEST and RESPONSE frames it exchanges on the reliable
 * channel. */
#include <string.h>

#include "bytes.h"
#include "fake_port.h"
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_services.h"
#include "r2s/r2s_session.h"
#include "r2s/r2s_tx.h"
#include "unity.h"

/* One frame at the encoded limit, plus its delimiter. */
#define WIRE_MAX (R2S_FRAMER_RX_CAPACITY + 1u)

static uint8_t wire[WIRE_MAX];

static const r2s_topic_t one_topic[] = {
  {0x10u, 0x00u, {0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u}, "std_msgs/msg/Float32",
    "float"},
};

/* The announced table: a served reset and a called trigger. */
static r2s_service_t table[] = {
  {"std_srvs/srv/Trigger",
    "reset",
    R2S_SVC_SERVER,
    {0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u, 0x08u}},
  {"std_srvs/srv/Trigger",
    "trigger",
    R2S_SVC_CLIENT,
    {0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u}},
};

static void configure(void)
{
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_session_configure("node", one_topic, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_services_configure(table, 2));
  TEST_ASSERT_EQUAL_UINT8(2, r2s_services_count());
  r2s_init();
}

/* Checks the index-th captured frame against the wire form of its topic, seq
 * and payload, the same comparison the frames conformance test makes. */
static void assert_tx(
  uint16_t index, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
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

/* Moves to CONNECTED the way the handshake does: READY with a nonzero
 * token, answered in ANNOUNCING. */
static void connect(void)
{
  const uint8_t ready[] = {0x06u, 0x01u, 0x00u, 0x00u, 0x00u};

  feed_host(0, 0, ready, sizeof ready);
  TEST_ASSERT_EQUAL_UINT8(1, r2s_connected());
}

/* Keeps the link alive across delay_ms, feeding one host heartbeat per 500
 * ms tick the way a connected host does. */
static void keep_alive(uint32_t delay_ms)
{
  const uint8_t empty[1] = {0u};
  uint8_t seq = 0u;
  uint32_t left = delay_ms;

  while (left > 0u) {
    fake_port_advance(500);
    feed_host(4, seq, empty, 0);
    seq = (uint8_t)(seq + 1u);
    left -= 500u;
  }
}

/* covers: SVR-1, SVR-2, SVR-3 */
void test_services_configure_refuses_a_table_it_cannot_announce(void)
{
  static r2s_service_t bad_role[] = {
    {"std_srvs/srv/Trigger", "reset", 9u, {0u}},
  };
  static r2s_service_t null_type[] = {
    {NULL, "reset", R2S_SVC_SERVER, {0u}},
  };
  static r2s_service_t null_name[] = {
    {"std_srvs/srv/Trigger", NULL, R2S_SVC_SERVER, {0u}},
  };
  static char long_name[600];
  static r2s_service_t long_table[] = {
    {"std_srvs/srv/Trigger", "", R2S_SVC_SERVER, {0u}},
  };
  static char huge_name[65537];
  static r2s_service_t huge_table[] = {
    {"t", "x", R2S_SVC_SERVER, {0u}},
  };
  static char huge_type[65537];
  static r2s_service_t huge_type_table[] = {
    {"t", "reset", R2S_SVC_SERVER, {0u}},
  };

  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_services_configure(NULL, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_services_configure(bad_role, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_services_configure(null_type, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_services_configure(null_name, 1));
  memset(long_name, 'n', sizeof long_name - 1u);
  long_name[sizeof long_name - 1u] = '\0';
  long_table[0].service_name = long_name;
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_services_configure(long_table, 1));
  memset(huge_name, 'n', sizeof huge_name - 1u);
  huge_name[sizeof huge_name - 1u] = '\0';
  huge_table[0].service_name = huge_name;
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_services_configure(huge_table, 1));
  memset(huge_type, 't', sizeof huge_type - 1u);
  huge_type[sizeof huge_type - 1u] = '\0';
  huge_type_table[0].type_name = huge_type;
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_services_configure(huge_type_table, 1));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_services_configure(NULL, 0));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_services_count());
  TEST_ASSERT_NULL(r2s_services_get(0));
  TEST_ASSERT_EQUAL_UINT8(0xFFu, r2s_services_status(0));
  TEST_ASSERT_EQUAL_UINT16(0, r2s_services_declare_len(0));
}

/* covers: SVR-1, SVR-2, SVR-3, SVR-7 */
void test_services_query_answers_declare_in_announcing(void)
{
  const uint8_t req[] = {0x01u, 0x00u};
    /* DECLARE index 0, server role, the hash, "std_srvs/srv/Trigger" and
     * "reset". */
  const uint8_t declare[] = {0x02u, 0x00u, 0x00u, 0x01u, 0x02u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
    0x08u, 0x14u, 's', 't', 'd', '_', 's', 'r', 'v', 's',
    '/', 's', 'r', 'v', '/', 'T', 'r', 'i', 'g', 'g',
    'e', 'r', 0x05u, 'r', 'e', 's', 'e', 't'};

  configure();
  TEST_ASSERT_EQUAL_UINT16(38, r2s_services_declare_len(0));
  feed_host(7, 0, req, sizeof req);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 7, 0, declare, sizeof declare);
}

/* covers: SVR-1, SVR-7 */
void test_services_query_past_the_table_answers_status_1_plain(void)
{
  const uint8_t req[] = {0x01u, 0x02u};
    /* A plain reply under the request retry carries rel_seq 0. */
  const uint8_t status[] = {0x03u, 0x00u, 0x02u, 0x01u};

  configure();
  feed_host(7, 0, req, sizeof req);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 7, 0, status, sizeof status);
}

/* covers: SVR-9 */
void test_services_request_in_announcing_answers_hello_and_ignores(void)
{
    /* REQUEST rel 0, req 7, index 0, empty Trigger request. */
  const uint8_t request[] = {0x04u, 0x00u, 0x07u, 0x00u};
  uint8_t index = 0xFFu;
  uint8_t req_id = 0xFFu;
  uint8_t payload[8];
  uint16_t len = 0xFFFFu;

  configure();
  feed_host(7, 0, request, sizeof request);
    /* The HELLO nack is the only answer, and nothing waits for the
     * application. */
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_EMPTY,
                            r2s_service_take_request(&index, &req_id, payload, sizeof payload,
    &len));
  TEST_ASSERT_EQUAL_UINT8(0, r2s_connected());
}

/* covers: SVR-4, SVR-5, REL-1, REL-3 */
void test_services_server_request_applies_once_and_answers(void)
{
    /* REQUEST rel 0, req 7, index 0, empty Trigger request. */
  const uint8_t request[] = {0x04u, 0x00u, 0x07u, 0x00u};
  const uint8_t ack[] = {0x07u, 0x00u};
    /* The duplicate carries another value but the same rel seq. */
  const uint8_t resend[] = {0x04u, 0x00u, 0x07u, 0x00u};
    /* RESPONSE rel 0, req 7, ok, success true and "ok". */
  const uint8_t response[] = {0x05u, 0x00u, 0x07u, 0x00u, 0x01u, 0x02u, 'o', 'k'};
  uint8_t index = 0xFFu;
  uint8_t req_id = 0xFFu;
  uint8_t payload[8];
  uint16_t len = 0xFFFFu;
  const uint8_t answer[] = {0x01u, 0x02u, 'o', 'k'};

  configure();
  connect();
  fake_port_tx_clear();
  feed_host(7, 0, request, sizeof request);
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
    /* The ack names the channel and the rel_seq of the frame received. */
  assert_tx(0, 6, 0, ack, sizeof ack);
  TEST_ASSERT_EQUAL_INT8(R2S_OK,
    r2s_service_take_request(&index, &req_id, payload, sizeof payload, &len));
  TEST_ASSERT_EQUAL_UINT8(0, index);
  TEST_ASSERT_EQUAL_UINT8(7, req_id);
  TEST_ASSERT_EQUAL_UINT16(0, len);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_EMPTY,
                            r2s_service_take_request(&index, &req_id, payload, sizeof payload,
    &len));
    /* The duplicate is acked again on the next ack seq; nothing applies. */
  feed_host(7, 1, resend, sizeof resend);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 6, 1, ack, sizeof ack);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_EMPTY,
                            r2s_service_take_request(&index, &req_id, payload, sizeof payload,
    &len));
    /* The RESPONSE rides the reliable channel with its own rel seq. */
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_service_send_response(7, R2S_SVC_OK, answer, sizeof answer));
  TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
  assert_tx(2, 7, 0, response, sizeof response);
}

/* covers: SVR-6 */
void test_services_send_response_refuses_a_bad_status(void)
{
  const uint8_t answer[] = {0x01u};

  configure();
  connect();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_service_send_response(0, 9u, answer, sizeof answer));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_service_send_response(0, R2S_SVC_OK, NULL, 1u));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}

/* covers: SVR-1, SVR-4 */
void test_services_request_for_a_client_role_is_refused(void)
{
    /* REQUEST rel 0, req 3, index 1: the host calls a service the MCU
     * announced as one it calls itself. */
  const uint8_t request[] = {0x04u, 0x00u, 0x03u, 0x01u};
  uint8_t index = 0xFFu;
  uint8_t req_id = 0xFFu;
  uint8_t payload[8];
  uint16_t len = 0xFFFFu;

  configure();
  connect();
  fake_port_tx_clear();
  feed_host(7, 0, request, sizeof request);
    /* Refused without an ack, like an undecodable SET: nothing answers. */
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_EMPTY,
                            r2s_service_take_request(&index, &req_id, payload, sizeof payload,
    &len));
}

/* covers: SVR-4, SVR-5, SVR-6, REL-2 */
void test_services_client_call_round_trip(void)
{
    /* REQUEST rel 0, req 0, index 1, empty Trigger request. */
  const uint8_t want[] = {0x04u, 0x00u, 0x00u, 0x01u};
    /* RESPONSE rel 0, req 0, ok, success true and "ok". */
  const uint8_t response[] = {0x05u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 'o', 'k'};
  const uint8_t ack[] = {0x07u, 0x00u};
  uint8_t req_id = 0xFFu;
  uint8_t status = 0xFFu;
  uint8_t payload[8];
  uint16_t len = 0xFFFFu;
  const uint8_t answer[] = {0x01u, 0x02u, 'o', 'k'};

  configure();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_service_call(1, NULL, 0, &req_id));
  TEST_ASSERT_EQUAL_UINT8(0, req_id);
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 7, 0, want, sizeof want);
    /* The host ack frees the stream, and its RESPONSE answers the call. */
  feed_host(6, 0, ack, sizeof ack);
  feed_host(7, 0, response, sizeof response);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 6, 0, ack, sizeof ack);
  TEST_ASSERT_EQUAL_INT8(R2S_OK,
                           r2s_service_take_response(&req_id, &status, payload, sizeof payload,
    &len));
  TEST_ASSERT_EQUAL_UINT8(0, req_id);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_OK, status);
  TEST_ASSERT_EQUAL_UINT16(4, len);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(answer, payload, sizeof answer);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_EMPTY,
                            r2s_service_take_response(&req_id, &status, payload, sizeof payload,
    &len));
}

/* covers: SVR-5 */
void test_services_stale_response_is_acked_and_dropped(void)
{
    /* RESPONSE rel 0, req 9, ok: no call under req 9 is outstanding. */
  const uint8_t response[] = {0x05u, 0x00u, 0x09u, 0x00u};
  const uint8_t ack[] = {0x07u, 0x00u};
  uint8_t req_id = 0xFFu;
  uint8_t status = 0xFFu;
  uint8_t payload[8];
  uint16_t len = 0xFFFFu;

  configure();
  connect();
  fake_port_tx_clear();
  feed_host(7, 0, response, sizeof response);
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 6, 0, ack, sizeof ack);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_EMPTY,
                            r2s_service_take_response(&req_id, &status, payload, sizeof payload,
    &len));
}

/* covers: SVR-9, ERR-3 */
void test_services_client_call_while_not_connected_reports_once(void)
{
    /* code 5, services, one occurrence, no field. */
  const uint8_t report[] = {0x05u, 0x07u, 0x01u, 0x00u, 0xFFu};
  const uint8_t hello_req[] = {0x01u, 0x04u};
  uint8_t req_id = 0xFFu;

  configure();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, r2s_service_call(1, NULL, 0, &req_id));
    /* The failure reports at once: boot HELLO, then the report. */
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 5, 0, report, sizeof report);
  feed_host(0, 0, hello_req, sizeof hello_req);
  TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
    /* A second failure while still announcing counts without sending again. */
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, r2s_service_call(1, NULL, 0, &req_id));
  feed_host(0, 1, hello_req, sizeof hello_req);
  TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
}

/* covers: SVR-9 */
void test_services_client_call_while_not_connected_fails(void)
{
    /* code 5, services, one occurrence, no field. */
  const uint8_t report[] = {0x05u, 0x07u, 0x01u, 0x00u, 0xFFu};
  uint8_t req_id = 0xFFu;

  configure();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED, r2s_service_call(1, NULL, 0, &req_id));
    /* Nothing was sent but the boot HELLO and the once-per-connection
     * report. */
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 5, 0, report, sizeof report);
}

/* covers: SVR-9 */
void test_services_send_response_while_not_connected_fails(void)
{
    /* code 5, services, one occurrence, no field. */
  const uint8_t report[] = {0x05u, 0x07u, 0x01u, 0x00u, 0xFFu};
  const uint8_t answer[] = {0x01u};

  configure();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_NOT_CONNECTED,
                           r2s_service_send_response(0, R2S_SVC_OK, answer, sizeof answer));
    /* Nothing was sent but the boot HELLO and the once-per-connection
     * report. */
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(1, 5, 0, report, sizeof report);
}

/* covers: SVR-10 */
void test_services_second_call_while_one_is_outstanding_is_busy(void)
{
    /* REQUEST rel 0, req 0, index 1. */
  const uint8_t want[] = {0x04u, 0x00u, 0x00u, 0x01u};
  uint8_t req_id = 0xFFu;

  configure();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_service_call(1, NULL, 0, &req_id));
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_TX_BUSY, r2s_service_call(1, NULL, 0, &req_id));
    /* The second call sent nothing. */
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 7, 0, want, sizeof want);
}

/* covers: SVR-8 */
void test_services_call_timeout_retires_the_req_id(void)
{
    /* RESPONSE rel 0, req 0, ok: arriving after the call timed out. */
  const uint8_t late[] = {0x05u, 0x00u, 0x00u, 0x00u};
  const uint8_t ack[] = {0x07u, 0x00u};
  uint8_t req_id = 0xFFu;
  uint8_t status = 0xFFu;
  uint8_t payload[8];
  uint16_t len = 0xFFFFu;

  configure();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_service_call(1, NULL, 0, &req_id));
  TEST_ASSERT_EQUAL_UINT8(0, req_id);
    /* The fed heartbeats keep the link alive, so the call, not the link,
     * times out. */
  keep_alive(5000);
  TEST_ASSERT_EQUAL_INT8(R2S_OK,
                           r2s_service_take_response(&req_id, &status, payload, sizeof payload,
    &len));
  TEST_ASSERT_EQUAL_UINT8(0, req_id);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_TIMEOUT, status);
  TEST_ASSERT_EQUAL_UINT16(0, len);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_EMPTY,
                            r2s_service_take_response(&req_id, &status, payload, sizeof payload,
    &len));
    /* The late RESPONSE is still acked, then dropped. */
  {
    uint16_t n = fake_port_tx_frame_count();

    feed_host(7, 10, late, sizeof late);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(n + 1u), fake_port_tx_frame_count());
    assert_tx(n, 6, 0, ack, sizeof ack);
  }
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_EMPTY,
                            r2s_service_take_response(&req_id, &status, payload, sizeof payload,
    &len));
}

/* covers: REL-2, REL-4, HB-7 */
void test_services_response_retransmits_then_gives_up_counted(void)
{
  const uint8_t request[] = {0x04u, 0x00u, 0x07u, 0x00u};
  const uint8_t response[] = {0x05u, 0x00u, 0x07u, 0x00u, 0x01u};
  const uint8_t empty[1] = {0u};
  uint8_t index = 0xFFu;
  uint8_t req_id = 0xFFu;
  uint8_t payload[8];
  uint16_t len = 0xFFFFu;
  const uint8_t answer[] = {0x01u};

  configure();
  connect();
  fake_port_tx_clear();
  feed_host(7, 0, request, sizeof request);
  TEST_ASSERT_EQUAL_INT8(R2S_OK,
    r2s_service_take_request(&index, &req_id, payload, sizeof payload, &len));
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_service_send_response(7, R2S_SVC_OK, answer, sizeof answer));
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
    /* Three retries with the same rel seq, then the drop is counted: the fed
     * heartbeat keeps the link alive while the timer fires. Each pass also
     * sends the heartbeat, so the resend lands two frames before the next. */
  for (uint16_t retry = 1u; retry <= 3u; retry++) {
    fake_port_advance(500);
    feed_host(4, (uint8_t)(retry - 1u), empty, 0);
    assert_tx((uint16_t)(2u * retry), 7, (uint8_t)retry, response, sizeof response);
  }
  {
        /* The fed heartbeat is still in flight when the timer fires, so
         * the report counts the five frames accepted before it. */
    uint8_t want[8] = {5u, 0u, 0u, 0u, 0u, 0u, 1u, 0u};

    fake_port_advance(500);
    feed_host(4, 3, empty, 0);
    assert_tx(8, 4, 3, want, sizeof want);
  }
}

/* covers: REL-3 */
void test_services_channels_keep_their_own_rel_seq(void)
{
    /* SET rel 0, index 0, false: the parameter STATUS stays outstanding. */
  const uint8_t set[] = {0x03u, 0x00u, 0x00u, 0x00u};
  const uint8_t request[] = {0x04u, 0x00u, 0x07u, 0x00u};
  const uint8_t ack_param[] = {0x03u, 0x00u};
  const uint8_t ack_svc[] = {0x07u, 0x00u};
    /* RESPONSE rel 0, req 7, ok. */
  const uint8_t response[] = {0x05u, 0x00u, 0x07u, 0x00u, 0x01u};
  uint8_t index = 0xFFu;
  uint8_t req_id = 0xFFu;
  uint8_t payload[8];
  uint16_t len = 0xFFFFu;
  const uint8_t answer[] = {0x01u};

  configure();
  connect();
  fake_port_tx_clear();
  feed_host(3, 0, set, sizeof set);
  feed_host(7, 0, request, sizeof request);
  TEST_ASSERT_EQUAL_INT8(R2S_OK,
    r2s_service_take_request(&index, &req_id, payload, sizeof payload, &len));
    /* The RESPONSE goes out at once, on its own stream: the unacked STATUS
     * does not block it. */
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_service_send_response(7, R2S_SVC_OK, answer, sizeof answer));
  TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
  assert_tx(2, 6, 1, ack_svc, sizeof ack_svc);
  assert_tx(3, 7, 0, response, sizeof response);
    /* Each channel clears on its own ack, with no further traffic: the fed
     * heartbeat is the only frame that follows. */
  {
    const uint8_t empty[1] = {0u};
        /* rx_frames counts READY, SET, REQUEST and the two acks; the fed
         * heartbeat is still in flight, and nothing was dropped. */
    const uint8_t want[8] = {5u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};

    feed_host(6, 2, ack_param, sizeof ack_param);
    feed_host(6, 3, ack_svc, sizeof ack_svc);
    TEST_ASSERT_EQUAL_UINT16(4, fake_port_tx_frame_count());
    fake_port_advance(600);
    feed_host(4, 0, empty, 0);
    TEST_ASSERT_EQUAL_UINT16(5, fake_port_tx_frame_count());
    assert_tx(4, 4, 0, want, sizeof want);
  }
}

/* covers: SVR-7 */
void test_services_walk_status_is_stored_per_service(void)
{
  const uint8_t req[] = {0x01u, 0x00u};
    /* STATUS rel 0, index 0, ok: the host's verdict on the DECLARE. */
  const uint8_t verdict[] = {0x03u, 0x00u, 0x00u, 0x00u};

  configure();
  TEST_ASSERT_EQUAL_UINT8(0xFFu, r2s_services_status(0));
  feed_host(7, 0, req, sizeof req);
  feed_host(7, 1, verdict, sizeof verdict);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_STATUS_OK, r2s_services_status(0));
  TEST_ASSERT_EQUAL_UINT8(0xFFu, r2s_services_status(1));
}

/* covers: SVR-4 */
void test_services_take_request_with_a_small_buffer_keeps_the_slot(void)
{
    /* REQUEST rel 0, req 7, index 0, two payload bytes. */
  const uint8_t request[] = {0x04u, 0x00u, 0x07u, 0x00u, 0xAAu, 0xBBu};
  uint8_t index = 0xFFu;
  uint8_t req_id = 0xFFu;
  uint8_t payload[1];
  uint16_t len = 0xFFFFu;

  configure();
  connect();
  fake_port_tx_clear();
  feed_host(7, 0, request, sizeof request);
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_service_take_request(&index, &req_id, payload, sizeof payload,
    &len));
    /* The slot is kept, so a fitting buffer still takes the REQUEST. */
  {
    uint8_t room[8];
    uint16_t room_len = 0xFFFFu;

    TEST_ASSERT_EQUAL_INT8(R2S_OK,
                               r2s_service_take_request(&index, &req_id, room, sizeof room,
      &room_len));
    TEST_ASSERT_EQUAL_UINT8(7, req_id);
    TEST_ASSERT_EQUAL_UINT16(2, room_len);
  }
}

/* covers: SVR-5 */
void test_services_take_response_with_a_small_buffer_keeps_the_slot(void)
{
  const uint8_t response[] = {0x05u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u};
  uint8_t req_id = 0xFFu;
  uint8_t status = 0xFFu;
  uint8_t payload[1];
  uint16_t len = 0xFFFFu;
  const uint8_t ack[] = {0x07u, 0x00u};

  configure();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_service_call(1, NULL, 0, &req_id));
  feed_host(6, 0, ack, sizeof ack);
  feed_host(7, 0, response, sizeof response);
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS,
                           r2s_service_take_response(&req_id, &status, payload, sizeof payload,
    &len));
    /* The slot is kept, so a fitting buffer still takes the outcome. */
  {
    uint8_t room[8];
    uint16_t room_len = 0xFFFFu;

    TEST_ASSERT_EQUAL_INT8(R2S_OK,
                               r2s_service_take_response(&req_id, &status, room, sizeof room,
      &room_len));
    TEST_ASSERT_EQUAL_UINT8(R2S_SVC_OK, status);
    TEST_ASSERT_EQUAL_UINT16(2, room_len);
  }
}

/* covers: SVR-4 */
void test_services_call_with_an_oversize_payload_sends_nothing(void)
{
  static uint8_t big[R2S_MAX_FRAME];
  uint8_t req_id = 0xFFu;

  configure();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_ERR_BOUNDS, r2s_service_call(1, big, sizeof big, &req_id));
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

/* covers: SVR-6 */
void test_services_response_with_a_bad_status_is_refused(void)
{
    /* RESPONSE rel 0, req 0, status 9: reserved on the wire. */
  const uint8_t response[] = {0x05u, 0x00u, 0x00u, 0x09u};
  uint8_t req_id = 0xFFu;

  configure();
  connect();
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_service_call(1, NULL, 0, &req_id));
  feed_host(7, 0, response, sizeof response);
    /* Refused, not acked: the frame count only holds the call's REQUEST. */
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
}
