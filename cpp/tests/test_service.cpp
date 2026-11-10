/* The ROS 2-named service API over the core services channel: the MCU
 * serves reset and calls trigger (both std_srvs Trigger), and the announce,
 * the request bytes and the answer are the ones the core speaks. The bundled
 * tables carry the wire form, so no handwritten fixture stands in for a
 * message. */
#include <stdio.h>
#include <string.h>

/* The fake port's header carries no C linkage guard, and this file is C++. */
extern "C" {
#include "fake_port.h"
}
#include "frames.h"
#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_error.h"
#include "r2s/r2s_framer.h"
#include "r2s/r2s_services.h"
#include "r2s/r2s_session.h"
#include "r2s/runtime.hpp"
#include "r2s/service.hpp"
#include "ros2serial/std_msgs/Float32.h"
#include "std_msgs/msg/float32.hpp"
#include "std_srvs/srv/trigger.hpp"
#include "unity.h"

static uint8_t s_wire[R2S_FRAMER_RX_CAPACITY + 1u];

/* The service hash the bundle pins for std_srvs/srv/Trigger: comparing
 * against the traits would re-derive a miscapture. */
static const uint8_t s_trigger_hash[8] = {0xeeu, 0xffu, 0x2cu, 0xd6u, 0xfau, 0x5au, 0xd9u,
  0xd2u};

static uint8_t s_reset_calls;

static void on_reset(
  const std_srvs::srv::Trigger::Request & req, std_srvs::srv::Trigger::Response & res)
{
  (void)req;
  s_reset_calls++;
  res.success = 1u;
  res.message[0] = 'o';
  res.message[1] = 'k';
  res.message[2] = '\0';
}

/* Queues bytes one at a time, spinning after each, as a slow link
 * delivers. */
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
  uint16_t wire_len = r2s_test_frame(s_wire, topic_id, seq, payload, len);

  feed_slow(s_wire, wire_len);
}

/* Queues a host frame without spinning, so it waits until the sketch's own
 * spin serves it: a call's answer arrives while the call blocks. */
static void queue_host(uint8_t topic_id, uint8_t seq, const uint8_t *payload, uint16_t len)
{
  uint16_t wire_len = r2s_test_frame(s_wire, topic_id, seq, payload, len);

  fake_port_queue_rx(s_wire, wire_len);
}

/* The host's handshake: a hello request, a status per topic and the ready
 * frame that connects the session. */
static void handshake(uint8_t count)
{
  const uint8_t hello_req[] = {0x01u, R2S_PROTOCOL_VERSION};
  const uint8_t ready[] = {0x06u, 0x01u, 0x00u, 0x00u, 0x00u};
  uint8_t seq = 0u;
  uint8_t i;

  feed_host(0u, seq++, hello_req, sizeof hello_req);
  for (i = 0u; i < count; i++) {
    uint8_t status[] = {0x05u, (uint8_t)(16u + i), 0x00u};

    feed_host(0u, seq++, status, sizeof status);
  }
  feed_host(0u, seq++, ready, sizeof ready);
}

/* The index-th captured frame against the wire form of its payload. */
static void assert_tx(
  uint16_t index, uint8_t topic_id, uint8_t seq, const uint8_t *payload,
  uint16_t payload_len)
{
  static uint8_t expected[R2S_FRAMER_RX_CAPACITY + 1u];
  char label[64];
  uint16_t encoded_len;
  const uint8_t *encoded;
  uint16_t expected_len = r2s_test_frame(expected, topic_id, seq, payload, payload_len);

  snprintf(label, sizeof label, "tx frame %u", index);
  encoded = fake_port_tx_frame(index, &encoded_len);
  TEST_ASSERT_NOT_NULL_MESSAGE(encoded, label);
  TEST_ASSERT_EQUAL_UINT16(expected_len - 1u, encoded_len);
  TEST_ASSERT_EQUAL_MEMORY(expected, encoded, expected_len - 1u);
}

/* covers: SVR-2 */
void test_service_server_registers_the_table_entry(void)
{
  r2s::Node node("svc");
  std_msgs::msg::Float32 battery;
  r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");

  (void)battery;
  r2s::Service<std_srvs::srv::Trigger> reset(node, "reset", &on_reset);
  TEST_ASSERT_EQUAL_UINT8(0, reset.index());
  TEST_ASSERT_EQUAL_UINT8(1, r2s_services_count());
  TEST_ASSERT_EQUAL_STRING("std_srvs/srv/Trigger", r2s_services_get(0)->type_name);
  TEST_ASSERT_EQUAL_STRING("reset", r2s_services_get(0)->service_name);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_SERVER, r2s_services_get(0)->role);
  TEST_ASSERT_EQUAL_MEMORY(s_trigger_hash, r2s_services_get(0)->type_hash, 8u);
}

/* covers: SVR-4, SVR-5, SVR-6 */
void test_service_server_answers_a_request_with_the_callback_response(void)
{
  r2s::Node node("svc");
  std_msgs::msg::Float32 battery;
  r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
  r2s::Service<std_srvs::srv::Trigger> reset(node, "reset", &on_reset);
    /* REQUEST rel 0, req 9, index 0, empty Trigger request. */
  const uint8_t request[] = {0x04u, 0x00u, 0x09u, 0x00u};
  const uint8_t ack[] = {0x07u, 0x00u};
    /* RESPONSE rel 0, req 9, ok, success true and "ok". */
  const uint8_t response[] = {0x05u, 0x00u, 0x09u, 0x00u, 0x01u, 0x02u, 'o', 'k'};

  (void)battery;
  s_reset_calls = 0u;
  node.spin_some();
  handshake(1u);
  fake_port_tx_clear();
  feed_host(7u, 0u, request, sizeof request);
  node.spin_some();
  TEST_ASSERT_EQUAL_UINT8(1, s_reset_calls);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(0, 6u, 0u, ack, sizeof ack);
  assert_tx(1, 7u, 0u, response, sizeof response);
}

/* covers: SVR-4 */
void test_service_server_ignores_a_request_for_an_unknown_index(void)
{
  r2s::Node node("svc");
  std_msgs::msg::Float32 battery;
  r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
  r2s::Service<std_srvs::srv::Trigger> reset(node, "reset", &on_reset);
    /* REQUEST rel 0, req 9, index 5: no entry answers it. */
  const uint8_t request[] = {0x04u, 0x00u, 0x09u, 0x05u};

  (void)battery;
  s_reset_calls = 0u;
  node.spin_some();
  handshake(1u);
  fake_port_tx_clear();
  feed_host(7u, 0u, request, sizeof request);
  node.spin_some();
  TEST_ASSERT_EQUAL_UINT8(0, s_reset_calls);
    /* Refused without an ack, like an undecodable SET: nothing answers. */
  TEST_ASSERT_EQUAL_UINT16(0, fake_port_tx_frame_count());
}

void test_service_register_after_the_first_spin_is_refused(void)
{
  r2s::Node node("svc");
  std_msgs::msg::Float32 battery;
  r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
  r2s::Service<std_srvs::srv::Trigger> reset(node, "reset", &on_reset);

  (void)battery;
  node.spin_some();
  r2s::Service<std_srvs::srv::Trigger> late(node, "late", &on_reset);
  TEST_ASSERT_EQUAL_UINT8(0xFFu, late.index());
  TEST_ASSERT_TRUE(r2s::service::registration_error());
  TEST_ASSERT_EQUAL_UINT8(1, r2s_services_count());
}

/* covers: SVR-2 */
void test_service_client_registers_the_call_entry(void)
{
  r2s::Node node("svc");
  std_msgs::msg::Float32 battery;
  r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");

  (void)battery;
  r2s::Client<std_srvs::srv::Trigger> trigger(node, "trigger");
  TEST_ASSERT_EQUAL_UINT8(0, trigger.index());
  TEST_ASSERT_EQUAL_UINT8(1, r2s_services_count());
  TEST_ASSERT_EQUAL_STRING("std_srvs/srv/Trigger", r2s_services_get(0)->type_name);
  TEST_ASSERT_EQUAL_STRING("trigger", r2s_services_get(0)->service_name);
  TEST_ASSERT_EQUAL_UINT8(R2S_SVC_CLIENT, r2s_services_get(0)->role);
  TEST_ASSERT_EQUAL_MEMORY(s_trigger_hash, r2s_services_get(0)->type_hash, 8u);
}

/* covers: SVR-5, SVR-6 */
void test_service_client_call_returns_the_host_answer(void)
{
  r2s::Node node("svc");
  std_msgs::msg::Float32 battery;
  r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
  r2s::Client<std_srvs::srv::Trigger> trigger(node, "trigger");
    /* REQUEST rel 0, req 0, index 0, empty Trigger request. */
  const uint8_t want[] = {0x04u, 0x00u, 0x00u, 0x00u};
  const uint8_t ack[] = {0x07u, 0x00u};
    /* RESPONSE rel 0, req 0, ok, success true and "ok". */
  const uint8_t response[] = {0x05u, 0x00u, 0x00u, 0x00u, 0x01u, 0x02u, 'o', 'k'};
  std_srvs::srv::Trigger::Request req;
  std_srvs::srv::Trigger::Response res;

  (void)battery;
  memset(&req, 0, sizeof req);
  memset(&res, 0, sizeof res);
  node.spin_some();
  handshake(1u);
  fake_port_tx_clear();
  queue_host(6u, 0u, ack, sizeof ack);
  queue_host(7u, 0u, response, sizeof response);
  TEST_ASSERT_TRUE(trigger.call(req, res));
  TEST_ASSERT_EQUAL_UINT8(1u, res.success);
  TEST_ASSERT_EQUAL_STRING("ok", res.message);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(0, 7u, 0u, want, sizeof want);
  assert_tx(1, 6u, 0u, ack, sizeof ack);
}

/* covers: SVR-6 */
void test_service_client_call_reports_a_refused_status(void)
{
  r2s::Node node("svc");
  std_msgs::msg::Float32 battery;
  r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
  r2s::Client<std_srvs::srv::Trigger> trigger(node, "trigger");
  const uint8_t ack[] = {0x07u, 0x00u};
    /* RESPONSE rel 0, req 0, busy: the server took nothing. */
  const uint8_t response[] = {0x05u, 0x00u, 0x00u, 0x03u};
  std_srvs::srv::Trigger::Request req;
  std_srvs::srv::Trigger::Response res;

  (void)battery;
  memset(&req, 0, sizeof req);
  memset(&res, 0, sizeof res);
  node.spin_some();
  handshake(1u);
  fake_port_tx_clear();
  queue_host(6u, 0u, ack, sizeof ack);
  queue_host(7u, 0u, response, sizeof response);
  TEST_ASSERT_FALSE(trigger.call(req, res));
}

/* covers: SVR-9 */
void test_service_server_answer_retries_while_the_stream_is_busy(void)
{
  r2s::Node node("svc");
  std_msgs::msg::Float32 battery;
  r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
  r2s::Service<std_srvs::srv::Trigger> reset(node, "reset", &on_reset);
  r2s::Client<std_srvs::srv::Trigger> trigger(node, "trigger");
    /* REQUEST rel 0, req 9, index 0, empty Trigger request. */
  const uint8_t request[] = {0x04u, 0x00u, 0x09u, 0x00u};
  const uint8_t ack[] = {0x07u, 0x00u};
  const uint8_t want[] = {0x04u, 0x00u, 0x00u, 0x01u};
    /* RESPONSE rel 1, req 9, ok, success true and "ok". */
  const uint8_t response[] = {0x05u, 0x01u, 0x09u, 0x00u, 0x01u, 0x02u, 'o', 'k'};
  uint8_t req_id = 0xFFu;

  (void)battery;
  s_reset_calls = 0u;
  node.spin_some();
  handshake(1u);
  fake_port_tx_clear();
  TEST_ASSERT_EQUAL_INT8(R2S_OK, r2s_service_call(1, NULL, 0u, &req_id));
  feed_host(7u, 0u, request, sizeof request);
  node.spin_some();
  TEST_ASSERT_EQUAL_UINT8(1, s_reset_calls);
  TEST_ASSERT_EQUAL_UINT16(2, fake_port_tx_frame_count());
  assert_tx(0, 7u, 0u, want, sizeof want);
  assert_tx(1, 6u, 0u, ack, sizeof ack);
  feed_host(6u, 0u, ack, sizeof ack);
  node.spin_some();
  TEST_ASSERT_EQUAL_UINT16(3, fake_port_tx_frame_count());
  assert_tx(2, 7u, 1u, response, sizeof response);
}

/* covers: SVR-9 */
void test_service_client_call_refuses_while_disconnected(void)
{
  r2s::Node node("svc");
  std_msgs::msg::Float32 battery;
  r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");
  r2s::Client<std_srvs::srv::Trigger> trigger(node, "trigger");
  std_srvs::srv::Trigger::Request req;
  std_srvs::srv::Trigger::Response res;
    /* code 5, services, one occurrence, no field. */
  const uint8_t report[] = {0x05u, 0x07u, 0x01u, 0x00u, 0xFFu};

  (void)battery;
  memset(&req, 0, sizeof req);
  memset(&res, 0, sizeof res);
  node.spin_some();
  fake_port_tx_clear();
  TEST_ASSERT_FALSE(trigger.call(req, res));
  TEST_ASSERT_EQUAL_UINT16(1, fake_port_tx_frame_count());
  assert_tx(0, 5u, 0u, report, sizeof report);
}
