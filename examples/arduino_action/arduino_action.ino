// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

/* Arduino action example: the action server API on Uno and Mega, running
 * one goal. The action is the bundled LookupTransform action, so the goal,
 * result and feedback and the announced type come from the generated
 * header and the sketch names none of them. The sketch builds with
 * R2S_WITH_ACTIONS, which the bundled flags leave out. RAM cost: measured
 * by scripts/build-examples.sh. */

#include <ros2serial.h>

#include <string.h>

#include <r2s/action.hpp>
#include <tf2_msgs/action/lookup_transform.hpp>

/* The running goal, held between the accepted callback and the terminal
 * call, and the feedbacks sent so far. */
static r2s::action::ServerGoalHandle<tf2_msgs::action::LookupTransform> *s_goal = nullptr;
static uint8_t s_feedbacks = 0u;

r2s::action::GoalResponse on_spin_goal(const tf2_msgs::action::LookupTransform::Goal &goal)
{
    /* An empty target frame names no frame, so the example refuses it the
     * way a real server refuses a goal it cannot serve. */
    if (goal.target_frame[0] == '\0') {
        return r2s::action::GoalResponse::REJECT;
    }
    return r2s::action::GoalResponse::ACCEPT_AND_EXECUTE;
}

r2s::action::CancelResponse on_spin_cancel(const tf2_msgs::action::LookupTransform::Goal &goal)
{
    (void)goal;
    return r2s::action::CancelResponse::ACCEPT;
}

void on_spin_accepted(r2s::action::ServerGoalHandle<tf2_msgs::action::LookupTransform> &handle)
{
    s_goal = &handle;
    s_feedbacks = 0u;
}

r2s::Node node("base_controller");
r2s::action::Server<tf2_msgs::action::LookupTransform> spin("spin", on_spin_goal,
                                                            on_spin_cancel, on_spin_accepted);

void setup() { node.begin(Serial, 115200); }
void loop()
{
    node.spin_some();
    if (s_goal == nullptr) {
        return;
    }
    if (s_goal->is_canceling()) {
        tf2_msgs::action::LookupTransform::Result result;

        /* The wire form is the whole message, so every field starts known:
         * an unset stamp would ride the frame as stack bytes. */
        memset(&result, 0, sizeof result);
        (void)s_goal->canceled(result);
        s_goal = nullptr;
        return;
    }
    {
        tf2_msgs::action::LookupTransform::Feedback feedback;
        tf2_msgs::action::LookupTransform::Result result;

        feedback.structure_needs_at_least_one_member = 0u;
        (void)s_goal->publish_feedback(feedback);
        s_feedbacks++;
        if (s_feedbacks >= 3u) {
            /* The result carries a fixed transform, so the example shows a
             * payload a client reads rather than a zeroed one. */
            memset(&result, 0, sizeof result);
            result.transform.transform.translation.x = 1.0;
            result.transform.transform.translation.y = 2.0;
            result.transform.transform.translation.z = 3.0;
            result.transform.transform.rotation.w = 1.0;
            (void)s_goal->succeed(result);
            s_goal = nullptr;
        }
    }
}
