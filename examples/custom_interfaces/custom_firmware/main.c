// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Custom-type firmware: the generated static API on an ATmega328P. It
 * publishes the interface package's message, serves its service and runs its
 * action, so the walkthrough's interface reaches a real firmware. The
 * generated pair is emitted at build time from r2s.yaml beside this file. */
#include <avr/interrupt.h>
#include <avr/io.h>
#include <stdint.h>
#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_actions.h"
#include "r2s/r2s_port.h"
#include "r2s/r2s_services.h"
#include "r2s/r2s_session.h"
#include "r2s_generated.h"

/* 115200 baud from the 16 MHz crystal: the divisor truncates, keeping the
 * rate error under a percent, which the bridge tolerates. */
#define UBRR_VALUE (16000000UL / 16UL / 115200UL - 1UL)

/* Timer0 compare value for a 1 ms tick: 16 MHz / 64 / 250. */
#define TIMER0_TOP 249u

/* One feedback per this many milliseconds. Simulated time can outrun the
 * wall clock, so the window leaves the client time to observe it. */
#define FEEDBACK_PERIOD_MS 200u

/* The published reading's counter, carried in the message's status field. */
static uint16_t s_status;

/* Milliseconds since boot, advanced by the Timer0 compare interrupt. */
static volatile uint32_t s_ms;

/* Timer0 compare interrupt: the firmware's millisecond source. */
ISR(TIMER0_COMPA_vect)
{
  s_ms++;
}

/* Starts UART0 at 115200 8-N-1 and the millisecond tick. */
static void hardware_init(void)
{
  UBRR0 = (uint16_t)UBRR_VALUE;
  UCSR0B = (uint8_t)((1 << RXEN0) | (1 << TXEN0));
  UCSR0C = (uint8_t)((1 << UCSZ01) | (1 << UCSZ00));
  TCCR0A = (uint8_t)(1 << WGM01);
  TCCR0B = (uint8_t)((1 << CS01) | (1 << CS00));
  OCR0A = (uint8_t)TIMER0_TOP;
  TIMSK0 = (uint8_t)(1 << OCIE0A);
  sei();
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t count = 0u;

  while (count < max && (UCSR0A & (uint8_t)(1 << RXC0)) != 0u) {
    buf[count] = UDR0;
    count++;
  }
  return count;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    while ((UCSR0A & (uint8_t)(1 << UDRE0)) == 0u) {
    }
    UDR0 = buf[i];
  }
}

/* The write above polls for space, so it never refuses a byte. */
uint16_t r2s_port_tx_free(void)
{
  return 0xFFFFu;
}

/* The tick as milliseconds: read with interrupts off, since a 32-bit load
 * on AVR runs as four 8-bit loads that an interrupt could split. */
uint32_t r2s_port_tick(void)
{
  uint8_t flags = SREG;
  uint32_t now;

  cli();
  now = s_ms;
  SREG = flags;
  return now;
}

/* The generated service table sits in flash, and the core reads it as RAM,
 * so it is copied through the flash reader first (#632). Delete the copy
 * once the core learns the placement. */
static r2s_service_t s_services[R2S_SERVICE_COUNT];

static void copy_services_to_ram(void)
{
  uint8_t index;
  uint16_t byte;

  for (index = 0u; index < R2S_SERVICE_COUNT; index++) {
    for (byte = 0u; byte < (uint16_t)sizeof(r2s_service_t); byte++) {
      ((uint8_t *)&s_services[index])[byte] =
        R2S_FLASH_READ_BYTE((const uint8_t *)&r2s_services[index] + byte);
    }
  }
}

/* Serves calibrate: decode the request, answer with a note and an
 * acceptance that follows the target bits it decoded, so the caller reads a
 * value this firmware computed from the request. */
static void serve_calibrate(void)
{
  uint8_t index;
  uint8_t req_id;
  uint8_t payload[R2S_MAX_FRAME];
  uint16_t len = 0u;
  custom_interfaces__srv__Calibrate_Request request;
  custom_interfaces__srv__Calibrate_Response response;
  uint8_t field = 0xFFu;
  r2s_codec_out_t out = {payload, (uint16_t)sizeof payload, 0u, 0u};

  if (r2s_service_take_request(
      &index, &req_id, payload, (uint16_t)sizeof payload, &len) != R2S_OK)
  {
    return;
  }
  if (index != R2S_SERVICE_CALIBRATE_ID) {
    return;
  }
  memset(&request, 0, sizeof request);
  if (r2s_codec_decode(
      &r2s_generated_table, R2S_SERVICE_CALIBRATE_REQUEST_TYPE, payload, len, 0u, &request,
      &field) != R2S_OK)
  {
    (void)r2s_service_send_response(req_id, R2S_SVC_BAD_PAYLOAD, NULL, 0u);
    return;
  }
  memset(&response, 0, sizeof response);
  /* A nonzero target is accepted; the bits say so without a float compare. */
  {
    uint32_t bits;

    memcpy(&bits, &request.target, sizeof bits);
    response.accepted = (uint8_t)((bits & 0x7FFFFFFFu) != 0u ? 1u : 0u);
  }
  memcpy(response.note, "calibrated", 11u);
  if (r2s_codec_encode(
      &r2s_generated_table, R2S_SERVICE_CALIBRATE_RESPONSE_TYPE, &response, 0u, &out,
      &field) != R2S_OK)
  {
    (void)r2s_service_send_response(req_id, R2S_SVC_BAD_PAYLOAD, NULL, 0u);
    return;
  }
  (void)r2s_service_send_response(req_id, R2S_SVC_OK, payload, out.len);
}

/* The run action: one goal at a time. The feedback carries the order and
 * the result the value it produced, so the client sees both ends of it. */
static uint8_t s_goal_active;
static uint8_t s_goal_seq;
static float s_order;
static uint8_t s_feedbacks;
static uint32_t s_last_feedback;

/* Feedbacks before the result: a client that waits for one always sees it,
 * and the count keeps the two paced exchanges apart. */
#define FEEDBACK_COUNT 2u

void r2s_action_on_goal(uint8_t index, uint8_t goal_seq, const uint8_t *payload, uint16_t len)
{
  custom_interfaces__action__Run_Goal goal;
  uint8_t field = 0xFFu;

  if (index != R2S_ACTION_RUN_ID) {
    (void)r2s_actions_answer_goal(index, goal_seq, R2S_ACT_REJECTED);
    return;
  }
  memset(&goal, 0, sizeof goal);
  if (r2s_codec_decode(
      &r2s_generated_table, R2S_ACTION_RUN_GOAL_TYPE, payload, len, 0u, &goal,
      &field) != R2S_OK)
  {
    (void)r2s_actions_answer_goal(index, goal_seq, R2S_ACT_REJECTED);
    return;
  }
  s_order = goal.order;
  s_goal_seq = goal_seq;
  s_goal_active = 1u;
  s_feedbacks = 0u;
  s_last_feedback = r2s_port_tick();
  (void)r2s_actions_answer_goal(index, goal_seq, R2S_ACT_ACCEPTED);
}

void r2s_action_on_cancel(uint8_t index, uint8_t goal_seq)
{
  (void)index;
  (void)goal_seq;
}

static void send_feedback(void)
{
  custom_interfaces__action__Run_Feedback feedback;
  uint8_t payload[R2S_MAX_FRAME];
  uint8_t field = 0xFFu;
  r2s_codec_out_t out = {payload, (uint16_t)sizeof payload, 0u, 0u};

  memset(&feedback, 0, sizeof feedback);
  feedback.progress = s_order;
  if (r2s_codec_encode(
      &r2s_generated_table, R2S_ACTION_RUN_FEEDBACK_TYPE, &feedback, 0u, &out,
      &field) == R2S_OK)
  {
    (void)r2s_actions_feedback(R2S_ACTION_RUN_ID, s_goal_seq, payload, out.len);
  }
}

static void send_result(void)
{
  custom_interfaces__action__Run_Result result;
  uint8_t payload[R2S_MAX_FRAME];
  uint8_t field = 0xFFu;
  r2s_codec_out_t out = {payload, (uint16_t)sizeof payload, 0u, 0u};

  memset(&result, 0, sizeof result);
  result.value = s_order;
  memcpy(result.note, "done", 5u);
  if (r2s_codec_encode(
      &r2s_generated_table, R2S_ACTION_RUN_RESULT_TYPE, &result, 0u, &out,
      &field) != R2S_OK)
  {
    return;
  }
  if (r2s_actions_result(R2S_ACTION_RUN_ID, s_goal_seq, R2S_ACT_SUCCEEDED, payload,
    out.len) == R2S_OK)
  {
    s_goal_active = 0u;
  }
}

/* One feedback per period, then the result, so a client always observes
 * feedback before the goal ends. */
static void run_action(uint32_t now)
{
  if (s_goal_active == 0u) {
    return;
  }
  if ((uint32_t)(now - s_last_feedback) < FEEDBACK_PERIOD_MS) {
    return;
  }
  s_last_feedback = now;
  if (s_feedbacks < FEEDBACK_COUNT) {
    send_feedback();
    s_feedbacks++;
    return;
  }
  send_result();
}

static uint32_t s_last_pub;

/* One loop step, broken out so a host test drives it directly. */
void loop_once(void);

/* An example publishes a fresh reading, so a missed interval is dropped,
 * not replayed: the deadline moves one period per publish and resyncs to
 * now when more than one period late, and the offline branch resets it
 * the same way. A stall then skips its frames instead of bursting them. */
void loop_once(void)
{
  uint32_t now;

  r2s_spin_some();
  serve_calibrate();
  now = r2s_port_tick();
  run_action(now);
  if (r2s_connected() == 0u) {
    s_last_pub = now;
    return;
  }
  if ((uint32_t)(now - s_last_pub) >= 100u) {
    custom_interfaces__msg__SensorReading reading;

    reading.temperature = (float)s_status / 10.0f;
    reading.status = s_status;
    if (r2s_publish_reading(&reading) == R2S_OK) {
      s_status++;
      s_last_pub += 100u;
      if ((uint32_t)(now - s_last_pub) >= 100u) {
        s_last_pub = now;
      }
    }
  }
}

int main(void)
{
  hardware_init();
  copy_services_to_ram();
  if (r2s_session_configure("custom_board", r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
    return 1;
  }
  if (r2s_services_configure(s_services, R2S_SERVICE_COUNT) != R2S_OK) {
    return 1;
  }
  if (r2s_actions_configure(r2s_actions, R2S_ACTION_COUNT) != R2S_OK) {
    return 1;
  }
  r2s_init();
  s_last_pub = r2s_port_tick();
  for (;; ) {
    loop_once();
  }
}
