/* Cycle benchmark firmware: time the packed codec on representative narrow
 * payloads with Timer1 at one tick per cycle, and report over UART0. The
 * harness test runs this under simavr and pins the payload lengths and a
 * per-byte cycle budget. Encodes and decodes use buffer mode, so the port
 * stubs below only satisfy the linker. */

#include <avr/io.h>
#include <stdint.h>
#include <string.h>

#include "r2s/r2s.h"
#include "r2s/r2s_codec.h"
#include "r2s/r2s_port.h"
#include "r2s_bundled.h"

/* The tables live in r2s_bundled.c; their externs belong to the sketch
 * header's topic-list expansion, which this firmware does not use. */
extern const r2s_codec_table_t r2s_bundled_table_std_msgs_Float32;
extern const r2s_codec_table_t r2s_bundled_table_geometry_msgs_Twist;
extern const r2s_codec_table_t r2s_bundled_table_sensor_msgs_Range;
extern const r2s_codec_table_t r2s_bundled_table_sensor_msgs_Imu;
extern const r2s_codec_table_t r2s_bundled_table_sensor_msgs_JointState;

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  (void)buf;
  (void)max;
  return 0;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  (void)buf;
  (void)len;
}

uint16_t r2s_port_tx_free(void)
{
  return 0;
}

uint32_t r2s_port_tick(void)
{
  return 0;
}

static void uart_init(void)
{
  UBRR0H = 0;
  UBRR0L = 16;
  UCSR0A = (uint8_t)_BV(U2X0);
  UCSR0B = (uint8_t)_BV(TXEN0);
  UCSR0C = (uint8_t)(_BV(UCSZ01) | _BV(UCSZ00));
}

static void uart_putc(char c)
{
  while (!(UCSR0A & _BV(UDRE0))) {
  }
  UDR0 = (uint8_t)c;
}

static void uart_puts(const char *text)
{
  while (*text) {
    uart_putc(*text);
    text++;
  }
}

static void uart_putdec(uint16_t value)
{
  char digits[5];
  uint8_t count = 0;
  do {
    digits[count] = (char)('0' + value % 10u);
    count++;
    value /= 10u;
  } while (value && count < sizeof(digits));
  while (count) {
    count--;
    uart_putc(digits[count]);
  }
}

static uint8_t payload[R2S_MAX_FRAME];

static void report(const char *name, uint16_t enc, uint16_t dec, uint16_t len)
{
  uart_puts("BM ");
  uart_puts(name);
  uart_puts(" enc=");
  uart_putdec(enc);
  uart_puts(" dec=");
  uart_putdec(dec);
  uart_puts(" len=");
  uart_putdec(len);
  uart_puts("\r\n");
}

static void fail(const char *name, int8_t code)
{
  uart_puts("BM FAIL ");
  uart_puts(name);
  uart_putc(' ');
  uart_putc(code < 0 ? '-' : '+');
  uart_putdec((uint16_t)(code < 0 ? -code : code));
  uart_puts("\r\n");
}

static int8_t last_rc;

static uint16_t timed_encode(
  const r2s_codec_table_t *table, uint16_t index, const void *msg,
  r2s_codec_out_t *out)
{
  uint8_t field = 0;
  uint16_t start = TCNT1;
  int8_t rc = r2s_codec_encode(table, index, msg, 1, out, &field);
  uint16_t stop = TCNT1;
  last_rc = rc;
  if (rc != R2S_OK) {
    out->len = 0;
  }
  return (uint16_t)(stop - start);
}

static uint16_t timed_decode(
  const r2s_codec_table_t *table, uint16_t index, const uint8_t *bytes,
  uint16_t len, void *msg)
{
  uint8_t field = 0;
  uint16_t start = TCNT1;
  int8_t rc = r2s_codec_decode(table, index, bytes, len, 1, msg, &field);
  uint16_t stop = TCNT1;
  if (rc != R2S_OK) {
    fail("decode", rc);
  }
  return (uint16_t)(stop - start);
}

static void bench_float32(void)
{
  std_msgs__msg__Float32 msg;
  std_msgs__msg__Float32 back;
  r2s_codec_out_t out = {payload, sizeof(payload), 0, 0};
  uint16_t enc;
  uint16_t dec;

  msg.data = 1.5f;
  enc = timed_encode(&R2S_BUNDLED_TABLE_std_msgs_Float32, R2S_BUNDLED_INDEX_std_msgs_Float32, &msg,
    &out);
  if (!out.len) {
    fail("float32", last_rc);
    return;
  }
  dec = timed_decode(&R2S_BUNDLED_TABLE_std_msgs_Float32, R2S_BUNDLED_INDEX_std_msgs_Float32,
    payload,
                       out.len, &back);
  report("float32", enc, dec, out.len);
}

static void bench_twist(void)
{
  geometry_msgs__msg__Twist msg;
  geometry_msgs__msg__Twist back;
  r2s_codec_out_t out = {payload, sizeof(payload), 0, 0};
  uint16_t enc;
  uint16_t dec;

  msg.linear.x = 1.5;
  msg.linear.y = -2.0;
  msg.linear.z = 0.5;
  msg.angular.x = 1.0;
  msg.angular.y = -0.0;
  msg.angular.z = 3.0;
  enc = timed_encode(&R2S_BUNDLED_TABLE_geometry_msgs_Twist, R2S_BUNDLED_INDEX_geometry_msgs_Twist,
    &msg,
                       &out);
  if (!out.len) {
    fail("twist", last_rc);
    return;
  }
  dec = timed_decode(&R2S_BUNDLED_TABLE_geometry_msgs_Twist, R2S_BUNDLED_INDEX_geometry_msgs_Twist,
    payload,
                       out.len, &back);
  report("twist", enc, dec, out.len);
}

static void bench_range(void)
{
  sensor_msgs__msg__Range msg;
  sensor_msgs__msg__Range back;
  r2s_codec_out_t out = {payload, sizeof(payload), 0, 0};
  uint32_t bits;
  uint16_t enc;
  uint16_t dec;

  msg.header.stamp.sec = 1;
  msg.header.stamp.nanosec = 2;
  strcpy(msg.header.frame_id, "sonar");
  msg.radiation_type = 0;
  msg.field_of_view = 0.5f;
  bits = 0x3ca3d70aul;
  memcpy(&msg.min_range, &bits, sizeof(bits));
  msg.max_range = 4.0f;
  bits = 0x7f800000ul;
  memcpy(&msg.range, &bits, sizeof(bits));
  msg.variance = 0.0f;
  enc = timed_encode(&R2S_BUNDLED_TABLE_sensor_msgs_Range, R2S_BUNDLED_INDEX_sensor_msgs_Range,
    &msg, &out);
  if (!out.len) {
    fail("range", last_rc);
    return;
  }
  dec = timed_decode(&R2S_BUNDLED_TABLE_sensor_msgs_Range, R2S_BUNDLED_INDEX_sensor_msgs_Range,
    payload,
                       out.len, &back);
  report("range", enc, dec, out.len);
}

static void bench_imu(void)
{
  sensor_msgs__msg__Imu msg;
  sensor_msgs__msg__Imu back;
  r2s_codec_out_t out = {payload, sizeof(payload), 0, 0};
  uint16_t enc;
  uint16_t dec;
  uint8_t i;

  msg.header.stamp.sec = 100;
  msg.header.stamp.nanosec = 200;
  strcpy(msg.header.frame_id, "imu0");
  msg.orientation.x = 0.0;
  msg.orientation.y = 0.0;
  msg.orientation.z = 0.0;
  msg.orientation.w = 1.0;
  msg.orientation_covariance[0] = -1.0;
  msg.angular_velocity.x = 0.1;
  msg.angular_velocity.y = -0.2;
  msg.angular_velocity.z = 0.3;
  msg.linear_acceleration.x = 0.0;
  msg.linear_acceleration.y = 0.0;
  msg.linear_acceleration.z = 9.81;
  for (i = 1; i < 9; i++) {
    msg.orientation_covariance[i] = 0.0;
    msg.angular_velocity_covariance[i] = 0.0;
    msg.linear_acceleration_covariance[i] = 0.0;
  }
  msg.angular_velocity_covariance[0] = 0.0;
  msg.linear_acceleration_covariance[0] = 0.0;
  enc = timed_encode(&R2S_BUNDLED_TABLE_sensor_msgs_Imu, R2S_BUNDLED_INDEX_sensor_msgs_Imu, &msg,
    &out);
  if (!out.len) {
    fail("imu", last_rc);
    return;
  }
  dec = timed_decode(&R2S_BUNDLED_TABLE_sensor_msgs_Imu, R2S_BUNDLED_INDEX_sensor_msgs_Imu, payload,
                       out.len, &back);
  report("imu", enc, dec, out.len);
}

static void bench_joint_state(void)
{
  sensor_msgs__msg__JointState msg;
  sensor_msgs__msg__JointState back;
  r2s_codec_out_t out = {payload, sizeof(payload), 0, 0};
  uint16_t enc;
  uint16_t dec;

  msg.header.stamp.sec = 7;
  msg.header.stamp.nanosec = 8;
  strcpy(msg.header.frame_id, "base");
  msg.name.size = 2;
  strcpy(msg.name.data[0], "shoulder");
  strcpy(msg.name.data[1], "elbow");
  msg.position.size = 2;
  msg.position.data[0] = 1.5;
  msg.position.data[1] = -2.5;
  msg.velocity.size = 2;
  msg.velocity.data[0] = 0.0;
  msg.velocity.data[1] = 0.1;
  msg.effort.size = 0;
  enc = timed_encode(&R2S_BUNDLED_TABLE_sensor_msgs_JointState,
    R2S_BUNDLED_INDEX_sensor_msgs_JointState,
                       &msg, &out);
  if (!out.len) {
    fail("joint_state", last_rc);
    return;
  }
  dec = timed_decode(&R2S_BUNDLED_TABLE_sensor_msgs_JointState,
    R2S_BUNDLED_INDEX_sensor_msgs_JointState,
                       payload, out.len, &back);
  report("joint_state", enc, dec, out.len);
}

int main(void)
{
  uart_init();
  TCCR1A = 0;
  TCCR1B = (uint8_t)_BV(CS10);
  bench_float32();
  bench_twist();
  bench_range();
  bench_imu();
  bench_joint_state();
  uart_puts("BM DONE\r\n");
  for (;; ) {
  }
}
