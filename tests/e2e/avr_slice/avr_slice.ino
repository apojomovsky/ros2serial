// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Slice firmware for the avr end-to-end suite: the four topics the posix
 * virtual MCU announces, on an emulated Uno or Mega under simavr. The
 * sketch declares its topics in an R2S_TOPICS list, so no r2s.yaml and no
 * generator step are involved; battery publishes at the 50 Hz scenario 2
 * measures, and every cmd_vel frame echoes back for scenario 3. The spin
 * action serves scenario 15 through an R2S_ACTIONS row the same way. */

#define R2S_NODE_NAME "r2s_slice"
#define R2S_TOPICS(X) \
    X(Publisher, std_msgs::msg::Float32, battery_pub, "battery") \
    X(Subscription, geometry_msgs::msg::Twist, cmd_vel_sub, "cmd_vel", on_cmd_vel) \
    X(Publisher, geometry_msgs::msg::Twist, echo_vel_pub, "echo_vel") \
    X(Publisher, std_msgs::msg::String, log_text_pub, "log_text")

#define R2S_ACTIONS(X) \
    X(Spin, spin_srv, "spin", on_spin_goal, on_spin_cancel, on_spin_accepted)

#include <ros2serial.h>

#include <string.h>

#include <r2s/action.hpp>
#include <tf2_msgs/action/lookup_transform.hpp>

/* The spin action: the LookupTransform goal, result and feedback the bundle
 * carries, announced under this name. */
struct Spin {
    using Goal = tf2_msgs::action::LookupTransform_Goal;
    using Result = tf2_msgs::action::LookupTransform_Result;
    using Feedback = tf2_msgs::action::LookupTransform_Feedback;
};

namespace r2s
{
namespace action
{
template <>
struct traits<Spin> {
    static const char *action_name()
    {
        return "spin";
    }
    static const char *type_name()
    {
        return "tf2_msgs/action/LookupTransform";
    }
    static const uint8_t *type_hash()
    {
        /* First 8 bytes of the action's RIHS01 hash, as the generator
         * computes it (gen/tests/test_actions.py pins the full hash). */
        static const uint8_t hash[8] = {0x0bu, 0x8au, 0xdfu, 0x6bu, 0xc0u, 0xb5u, 0x95u, 0x88u};
        return hash;
    }
};
} // namespace action
} // namespace r2s

/* The running goal, held between the accepted callback and the terminal
 * call, paced like the posix firmware so the scenario's cancel, drop and
 * reset land inside the feedback window. */
static r2s::action::ServerGoalHandle<Spin> *s_goal = nullptr;
static uint8_t s_feedbacks = 0u;
static uint32_t s_last_feedback = 0u;

/* Fifty feedbacks at 200 ms: simulated time can outrun the wall clock
 * many times over, so the window must absorb the test's observation
 * latency and still hold the goal open for its cancel, drop and reset.
 * The posix firmware paces the same exchange in wall-clock time with a
 * shorter run. */
static const uint8_t kSpinFeedbacks = 50u;

r2s::action::GoalResponse on_spin_goal(const Spin::Goal &goal)
{
    (void)goal;
    return r2s::action::GoalResponse::ACCEPT_AND_EXECUTE;
}

r2s::action::CancelResponse on_spin_cancel(const Spin::Goal &goal)
{
    (void)goal;
    return r2s::action::CancelResponse::ACCEPT;
}

void on_spin_accepted(r2s::action::ServerGoalHandle<Spin> &handle)
{
    s_goal = &handle;
    s_feedbacks = 0u;
    s_last_feedback = r2s_port_tick();
}

R2S_ACTIONS(R2S_ACTION_X_SERVER);

/* The baud the sketch serves. The e2e suite overrides it per build to walk
 * the baud matrix; the default stays 115200. */
#ifndef R2S_BAUD
#define R2S_BAUD 115200
#endif

static uint32_t s_sent;
static uint8_t s_log_booted;

/* Best effort echo for scenario 3: five messages on an idle link never fill
 * the port, so the return carries no signal the test would use. */
void on_cmd_vel(const geometry_msgs::msg::Twist &msg)
{
    (void)r2s_publish_echo_vel_pub(&msg);
}

/* One log_text line: the value as 8 hex digits. Fixed width fits the
 * bundled String bound of 16 characters for every id. */
static r2s_status_t publish_log_text(uint32_t value)
{
    std_msgs::msg::String msg;
    uint8_t i;

    for (i = 0u; i < 8u; i++) {
        uint8_t digit = (uint8_t)((value >> 28u) & 0xFu);

        msg.data[i] = (char)(digit < 10u ? (char)('0' + digit) : (char)('a' + digit - 10u));
        value <<= 4u;
    }
    msg.data[8] = '\0';
    return r2s_publish_log_text_pub(&msg);
}

void setup(void)
{
    Serial.begin(R2S_BAUD);
    if (r2s_session_configure(R2S_NODE_NAME, r2s_topics, R2S_TOPIC_COUNT) != R2S_OK) {
        return;
    }
    r2s_init();
}

/* One goal at a time, paced like the posix firmware: an empty feedback
 * every 200 ms, then the fixed transform result. A cancel ends canceled. */
static void spin_some(uint32_t now)
{
    Spin::Feedback feedback;
    Spin::Result result;

    if (s_goal == nullptr) {
        return;
    }
    if (s_goal->is_canceling()) {
        memset(&result, 0, sizeof result);
        if (s_goal->canceled(result) == R2S_OK) {
            s_goal = nullptr;
        }
        return;
    }
    if ((uint32_t)(now - s_last_feedback) < 200u) {
        return;
    }
    s_last_feedback = now;
    if (s_feedbacks < kSpinFeedbacks) {
        memset(&feedback, 0, sizeof feedback);
        if (s_goal->publish_feedback(feedback) == R2S_OK) {
            s_feedbacks++;
        }
        return;
    }
    memset(&result, 0, sizeof result);
    memcpy(result.transform.header.frame_id, "odom", 5u);
    memcpy(result.transform.child_frame_id, "base", 5u);
    result.transform.transform.translation.x = 1.0;
    result.transform.transform.translation.y = 2.0;
    result.transform.transform.translation.z = 3.0;
    result.transform.transform.rotation.w = 1.0;
    if (s_goal->succeed(result) == R2S_OK) {
        s_goal = nullptr;
    }
}

void loop(void)
{
    static uint32_t last_pub;
    uint32_t now;

    r2s_spin_some();
    now = r2s_port_tick();
    if (r2s_connected() == 0u) {
        /* Offline there are no samples, so the clock resyncs instead of
         * queueing a burst for the reconnect. */
        last_pub = now;
        s_goal = nullptr;
        return;
    }
    if (s_log_booted == 0u) {
        /* A boot line that fails to publish is retried until it sends. It
         * carries the assigned session token, so the host sees what the
         * firmware stored. */
        if (publish_log_text(r2s_session_token()) == R2S_OK) {
            s_log_booted = 1u;
        }
    }
    spin_some(now);
    /* The slice mirrors the posix firmware, so it owes every missed tick:
     * one frame per missed interval keeps the count whole when the host
     * steals the firmware tick. */
    while ((uint32_t)(now - last_pub) >= 20u) {
        std_msgs::msg::Float32 msg;

        msg.data = (float)s_sent;
        if (r2s_publish_battery_pub(&msg) != R2S_OK) {
            break;
        }
        s_sent++;
        last_pub += 20u;
        if (s_sent % 10u == 0u) {
            (void)publish_log_text(s_sent);
        }
    }
}
