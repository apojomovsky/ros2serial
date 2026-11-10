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

/* Arduino action example: the action server API on Uno and Mega, running
 * one Fibonacci-like goal. The goal, result and feedback ride on the
 * bundled Float32 descriptors, so the example needs no r2s.yaml and no
 * generated output; the library ships the runtime layer and the bundled
 * message types. The sketch builds with R2S_WITH_ACTIONS, which the
 * bundled flags leave out. */

#include <ros2serial.h>

#include <r2s/action.hpp>
#include <std_msgs/msg/float32.hpp>

/* The Fibonacci-like action: the order to compute, the partial sequence
 * and the full one, all as Float32 payloads for the example. */
struct Spin {
    using Goal = std_msgs::msg::Float32;
    using Result = std_msgs::msg::Float32;
    using Feedback = std_msgs::msg::Float32;
};

namespace r2s
{
namespace action
{
template <> struct traits<Spin> {
    static const char *action_name()
    {
        return "spin";
    }
    static const char *type_name()
    {
        return "test_msgs/action/Fibonacci";
    }
    static const uint8_t *type_hash()
    {
        static const uint8_t hash[8] = {0x11u, 0x12u, 0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u};
        return hash;
    }
};
} // namespace action
} // namespace r2s

/* The running goal, held between the accepted callback and the terminal
 * call, and the feedback sent so far. */
static r2s::action::ServerGoalHandle<Spin> *s_goal = nullptr;
static uint8_t s_progress = 0u;

r2s::action::GoalResponse on_spin_goal(const std_msgs::msg::Float32 &goal)
{
    if (goal.data > 10.0f) {
        return r2s::action::GoalResponse::REJECT;
    }
    return r2s::action::GoalResponse::ACCEPT_AND_EXECUTE;
}

r2s::action::CancelResponse on_spin_cancel(const std_msgs::msg::Float32 &goal)
{
    (void)goal;
    return r2s::action::CancelResponse::ACCEPT;
}

void on_spin_accepted(r2s::action::ServerGoalHandle<Spin> &handle)
{
    s_goal = &handle;
    s_progress = 0u;
}

r2s::Node node("base_controller");
r2s::action::Server<Spin> fib("spin", on_spin_goal, on_spin_cancel, on_spin_accepted);

void setup() { node.begin(Serial, 115200); }
void loop()
{
    node.spin_some();
    if (s_goal == nullptr) {
        return;
    }
    if (s_goal->is_canceling()) {
        std_msgs::msg::Float32 result;

        result.data = 0.0f;
        (void)s_goal->canceled(result);
        s_goal = nullptr;
        return;
    }
    {
        std_msgs::msg::Float32 feedback;
        std_msgs::msg::Float32 result;

        feedback.data = (float)s_progress;
        (void)s_goal->publish_feedback(feedback);
        s_progress++;
        if (s_progress >= 3u) {
            result.data = 1.0f;
            (void)s_goal->succeed(result);
            s_goal = nullptr;
        }
    }
}
