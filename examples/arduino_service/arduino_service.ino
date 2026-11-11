// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Arduino service example: the service API on Uno and Mega, serving
 * reset and calling trigger (both std_srvs Trigger), the same pair the
 * C++ service test covers. The server answers reset with success and
 * the sketch's own text; the client calls trigger once per connection
 * and drops the answer, so the sketch shows both directions without a
 * second sketch. Try it with: ros2 service call /reset std_srvs/srv/Trigger
 * (bridge launch as in the README walkthrough). The sketch builds with
 * R2S_WITH_SERVICES, which the bundled flags leave out, and caps the
 * frame at 128: the bundle's 325-byte frames would leave the Uno no room
 * beside the service answer buffer. RAM cost: 1564 bytes on Uno, about
 * 0.3 KB over the minimal sketch (measured by scripts/build-examples.sh). */

#include <stdio.h>

#include <ros2serial.h>

#include <r2s/service.hpp>
#include <std_srvs/srv/trigger.hpp>

static void on_reset(
  const std_srvs::srv::Trigger::Request & req, std_srvs::srv::Trigger::Response & res)
{
    (void)req;
    res.success = 1u;
    snprintf(res.message, sizeof(res.message), "ok");
}

r2s::Node node("service_demo");
r2s::Service<std_srvs::srv::Trigger> reset_srv(node, "reset", on_reset);
r2s::Client<std_srvs::srv::Trigger> trigger_cli(node, "trigger");

/* The client calls once per connection: a second send while one is
 * outstanding fails busy, so the flag retires until the next connect. */
static bool s_trigger_done = false;

void setup() { node.begin(Serial, 115200); }
void loop()
{
    node.spin_some();
    if (!node.connected()) {
        s_trigger_done = false;
        return;
    }
    if (s_trigger_done) {
        return;
    }
    s_trigger_done = true;
    {
        std_srvs::srv::Trigger::Request req;
        std_srvs::srv::Trigger::Response res;

        req.structure_needs_at_least_one_member = 0u;
        (void)trigger_cli.call(req, res);
    }
}
