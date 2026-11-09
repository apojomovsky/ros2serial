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

/* C X-macro example: the bundled library on Uno and Mega, publishing
 * battery and greeting and counting cmd_vel and joints frames. The sketch
 * declares its topics in an R2S_TOPICS list with the generated C struct
 * names, so there is no r2s.yaml and no generator step; the bundled
 * descriptors compile in the library, which never sees the list. */

#define R2S_NODE_NAME "hello_xmacro_c"
#define R2S_TOPICS(X) \
  X(Publisher, std_msgs__msg__Float32, battery_pub, "battery") \
  X(Publisher, std_msgs__msg__String, greeting_pub, "greeting") \
  X(Subscription, geometry_msgs__msg__Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel) \
  X(Subscription, sensor_msgs__msg__JointState, joints_sub, "joints", on_joints)
#include <ros2serial.h>

#include "xmacro_c.h"

static uint32_t s_cmd_vel_count;
static uint32_t s_joints_count;
static uint32_t s_sent;

/* Handle cmd_vel and joints; dispatch calls them from r2s_spin_some(). */
void on_cmd_vel(const geometry_msgs__msg__Twist *msg)
{
  (void)msg;
  s_cmd_vel_count++;
}

void on_joints(const sensor_msgs__msg__JointState *msg)
{
  (void)msg;
  s_joints_count++;
}

void xmacro_setup(void)
{
  if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
    return;
  }
  r2s_init();
}

/* An example publishes a fresh reading, so a missed interval is dropped,
 * not replayed: the deadline moves one period per publish and resyncs to
 * now when more than one period late, and the offline branch resets it
 * the same way. A stall then skips its frames instead of bursting them. */
void xmacro_loop(void)
{
  static uint32_t last_pub;
  uint32_t now;

  r2s_spin_some();
  now = r2s_port_tick();
  if (r2s_connected() == 0u) {
    last_pub = now;
    return;
  }
  if ((uint32_t)(now - last_pub) >= 100u) {
    std_msgs__msg__Float32 battery = {(float)s_sent};
    std_msgs__msg__String greeting = {0};

    if (r2s_publish_battery_pub(&battery) == R2S_OK &&
      r2s_publish_greeting_pub(&greeting) == R2S_OK)
    {
      s_sent++;
      last_pub += 100u;
      if ((uint32_t)(now - last_pub) >= 100u) {
        last_pub = now;
      }
    }
  }
}
