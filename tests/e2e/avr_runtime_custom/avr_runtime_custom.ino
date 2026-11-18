// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Runtime custom-type firmware for the avr end-to-end suite: the interface
 * package's message, service and action through the C++ runtime API. The
 * types come from a generated type library, so no r2s.yaml and no generated
 * topic table are involved, and the action names the generated entity with
 * no hand-written traits. It mirrors examples/custom_interfaces, which runs
 * the same three kinds over the generated static C API. */

#include <ros2serial.h>

#include <string.h>

#include <r2s/action.hpp>
#include <r2s/service.hpp>

#include <custom_interfaces/action/run.hpp>
#include <custom_interfaces/msg/sensor_reading.hpp>
#include <custom_interfaces/srv/calibrate.hpp>

/* The published reading's counter, carried in the message's status field so
 * the host reads a decoded payload rather than a zeroed one. */
static uint16_t s_status;
static uint32_t s_last_pub;

r2s::Node node("custom_board");
r2s::Publisher<custom_interfaces::msg::SensorReading> reading_pub(node, "reading");

/* Serves calibrate: the answer follows the request bits it decoded, so the
 * caller reads a value this firmware computed from the request. */
static void on_calibrate(const custom_interfaces::srv::Calibrate::Request &req,
                         custom_interfaces::srv::Calibrate::Response &res)
{
    uint32_t bits;

    memcpy(&bits, &req.target, sizeof bits);
    res.accepted = (uint8_t)((bits & 0x7FFFFFFFu) != 0u ? 1u : 0u);
    memcpy(res.note, "calibrated", 11u);
}

r2s::Service<custom_interfaces::srv::Calibrate> calibrate_srv(node, "calibrate", on_calibrate);

/* The running goal, held between the accepted callback and the terminal
 * call. Ten feedbacks before the result, and a cancel ends the goal at the
 * next step, so a client always observes feedback first. */
static r2s::action::ServerGoalHandle<custom_interfaces::action::Run> *s_goal = nullptr;
static uint8_t s_feedbacks;
static uint32_t s_last_feedback;
static const uint8_t kFeedbackCount = 10u;

static r2s::action::GoalResponse on_run_goal(
    const custom_interfaces::action::Run::Goal &goal)
{
    /* A non-positive order names nothing to run, so the server refuses it
     * the way a real one refuses a goal it cannot serve. */
    uint32_t bits;

    memcpy(&bits, &goal.order, sizeof bits);
    if ((bits & 0x7FFFFFFFu) == 0u) {
        return r2s::action::GoalResponse::REJECT;
    }
    return r2s::action::GoalResponse::ACCEPT_AND_EXECUTE;
}

static r2s::action::CancelResponse on_run_cancel(
    const custom_interfaces::action::Run::Goal &goal)
{
    (void)goal;
    return r2s::action::CancelResponse::ACCEPT;
}

static void on_run_accepted(
    r2s::action::ServerGoalHandle<custom_interfaces::action::Run> &handle)
{
    s_goal = &handle;
    s_feedbacks = 0u;
    s_last_feedback = millis();
}

r2s::action::Server<custom_interfaces::action::Run> run("run", on_run_goal, on_run_cancel,
                                                        on_run_accepted);

void setup() { node.begin(Serial, 115200); }

/* One feedback per period, then the result. Both carry the goal's order, so
 * the client reads the value the firmware took from the goal. */
static void advance_goal(uint32_t now)
{
    custom_interfaces::action::Run::Result result;

    if (s_goal == nullptr) {
        return;
    }
    result.value = s_goal->get_goal().order;
    memcpy(result.note, "done", 5u);
    if (s_goal->is_canceling()) {
        result.value = 0.0f;
        if (s_goal->canceled(result) == R2S_OK) {
            s_goal = nullptr;
        }
        return;
    }
    if ((uint32_t)(now - s_last_feedback) < 200u) {
        return;
    }
    s_last_feedback = now;
    if (s_feedbacks < kFeedbackCount) {
        custom_interfaces::action::Run::Feedback feedback;

        feedback.progress = s_goal->get_goal().order;
        if (s_goal->publish_feedback(feedback) == R2S_OK) {
            s_feedbacks++;
        }
        return;
    }
    if (s_goal->succeed(result) == R2S_OK) {
        s_goal = nullptr;
    }
}

void loop()
{
    uint32_t now = millis();

    node.spin_some();
    if (!node.connected()) {
        /* Offline there are no samples, so the deadline resyncs instead of
         * queueing a burst for the reconnect. */
        s_last_pub = now;
        s_goal = nullptr;
        return;
    }
    if ((uint32_t)(now - s_last_pub) >= 100u) {
        custom_interfaces::msg::SensorReading reading;

        reading.temperature = (float)s_status / 10.0f;
        reading.status = s_status;
        if (reading_pub.publish(reading) == R2S_OK) {
            s_status++;
            s_last_pub = now;
        }
    }
    advance_goal(now);
}
