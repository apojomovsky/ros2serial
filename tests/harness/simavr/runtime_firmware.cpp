/* Runtime firmware for the simavr harness: the C++ runtime API on a real
 * AVR, announcing a Float32 topic and a Twist topic it subscribes to, and
 * echoing every Twist it receives on the battery topic. It links the core
 * and the runtime layer, so the handshake, the dispatch and the publish go
 * through the same code the board runs. */

#include <avr/io.h>
#include <stdint.h>

#include "r2s/runtime.hpp"

#include "geometry_msgs/msg/twist.hpp"
#include "std_msgs/msg/float32.hpp"

#define BAUD 115200UL
#define UBRR_VALUE (F_CPU / 16UL / BAUD - 1UL)

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

namespace
{
r2s::Node node("runtime_base");
r2s::Publisher<std_msgs::msg::Float32> battery_pub(node, "battery");

float s_linear;
bool s_received;

void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    s_linear = msg.linear.x;
    s_received = true;
}

r2s::Subscription<geometry_msgs::msg::Twist> cmd_vel_sub(node, "cmd_vel", on_cmd_vel);
} // namespace

int main(void)
{
    std_msgs::msg::Float32 battery;

    UBRR0 = (uint16_t)UBRR_VALUE;
    UCSR0B = (1 << RXEN0) | (1 << TXEN0);
    UCSR0C = (1 << UCSZ01) | (1 << UCSZ00);
    for (;;) {
        node.spin_some();
        if (s_received) {
            s_received = false;
            battery.data = s_linear;
            (void)battery_pub.publish(battery);
        }
    }
}
