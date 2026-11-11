// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Arduino service example: the service API on Uno and Mega, serving
 * reset and calling trigger (both std_srvs Trigger). The server answers
 * with success and the sketch's text; the client calls every few
 * seconds, showing both directions without a second sketch. Try: ros2
 * service call /reset std_srvs/srv/Trigger (bridge as in the README).
 * Builds with R2S_WITH_SERVICES, capped at 64-byte frames: the service
 * channel keeps three frame-sized buffers, and 128-byte frames crash the
 * Uno handshake. RAM cost: 1244 bytes on Uno. */

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

/* The client calls every few seconds while connected: a second send while
 * one is outstanding fails busy, so the wait also spaces the calls. The
 * call spins the node while it waits, so the server keeps answering. */
static unsigned long s_last_trigger;

void setup() { node.begin(Serial, 115200); }
void loop()
{
    node.spin_some();
    if (!node.connected() || millis() - s_last_trigger < 5000u) {
        return;
    }
    s_last_trigger = millis();
    {
        std_srvs::srv::Trigger::Request req;
        std_srvs::srv::Trigger::Response res;

        req.structure_needs_at_least_one_member = 0u;
        (void)trigger_cli.call(req, res);
    }
}
