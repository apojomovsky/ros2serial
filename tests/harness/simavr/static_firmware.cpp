/* Static firmware for the simavr harness: the static C++ API over an
 * R2S_TOPICS list, announcing a Float32 topic and a Twist topic it
 * subscribes to, and echoing every Twist it receives on the battery topic.
 * It links the core and the static dispatch, so the handshake, the dispatch
 * and the publish go through the same code the board runs. */

#include <avr/io.h>
#include <stdint.h>

#define R2S_TOPICS(X) \
  X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
  X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel)
#include "ros2serial.h"

extern "C" {
#include "r2s/r2s_port.h"
}

uint16_t r2s_port_read(uint8_t *buf, uint16_t max)
{
  uint16_t n = 0u;

  while (n < max && (UCSR0A & (1u << RXC0)) != 0u) {
    buf[n] = UDR0;
    n++;
  }
  return n;
}

void r2s_port_write(const uint8_t *buf, uint16_t len)
{
  uint16_t i;

  for (i = 0u; i < len; i++) {
    loop_until_bit_is_set(UCSR0A, UDRE0);
    UDR0 = buf[i];
  }
}

uint16_t r2s_port_tx_free(void)
{
  return 0xFFFFu;
}

/* The tick stays at zero: the test drives every reply from its own frames,
 * so no timer has to run and no link timeout can fire mid-test. */
uint32_t r2s_port_tick(void)
{
  return 0u;
}

r2s::Node node("static_base");

static float s_linear;
static bool s_received;

/* The dispatch calls the row's callback directly, so the echo state lives
 * in the statics above and the object below holds no storage. */
void on_cmd_vel(const geometry_msgs::msg::Twist & msg)
{
  s_linear = msg.linear.x;
  s_received = true;
}

#define BAUD 115200UL
#define UBRR_VALUE (F_CPU / 16UL / BAUD - 1UL)

int main(void)
{
  std_msgs::msg::Float32 battery;

  UBRR0 = (uint16_t)UBRR_VALUE;
  UCSR0B = (1 << RXEN0) | (1 << TXEN0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
  for (;; ) {
    node.spin_some();
    if (s_received) {
      s_received = false;
      battery.data = s_linear;
      (void)battery_pub.publish(battery);
    }
  }
}
