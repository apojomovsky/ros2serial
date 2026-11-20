# Parameters, services, and actions

Start with [topics](firmware.md). Optional channels add firmware storage and
build settings; their availability depends on the API and configuration you
compile. The basic Arduino helper prepares the bundled publish/subscribe path.
It does not automatically enable services, parameters, or actions.

The sketch interface here is the native one: `r2s::Service`, `r2s::Client` and
`r2s::action::Server` for C++, and the `r2s_services.h` and `r2s_actions.h`
calls for C. Which declaration style to start from is on
[choosing how to declare your topics](declaring-topics.md).

## Parameters

Firmware declares a parameter table before its first spin. With
`R2S_WITH_PARAMS` enabled in all firmware translation units, those declarations
become parameters on the bridge node; the sets travel over the reliable channel.
They are not parameters of a separate ROS node named after the MCU.

The bundled Arduino helper leaves the parameter channel out, so add the flag
the way the services recipe below adds its own: `-DR2S_WITH_PARAMS` in both
`compiler.c.extra_flags` and `compiler.cpp.extra_flags`.

The [migration guide's parameter section](03-port-rosserial-sketch.md#parameters)
shows a boolean `led` and a bounded string `label`, including the storage the
MCU owns.
After that firmware connects:

```sh
ros2 param list /r2s_bridge
ros2 param get /r2s_bridge led
ros2 param set /r2s_bridge led true
ros2 param set /r2s_bridge label 'front panel'
```

The update reaches the MCU through the reliable channel and is applied while it
spins. Under a namespace, address `/panel/r2s_bridge` instead. The button/LED
quickstart uses a `/led` topic, not this parameter; the commands above require
firmware declaring it.

## Services

Services support both directions: a ROS client calling an MCU server, and an MCU
client calling a ROS server. The `r2s::Service` and `r2s::Client` classes are the
sketch interface, and `examples/arduino_service` uses both. A rosserial sketch's
`advertiseService` and `serviceClient` port onto them; the
[migration guide](03-port-rosserial-sketch.md#services) maps each call.

Enable `R2S_WITH_SERVICES` in every firmware translation unit, as well as the
sketch. For an MCU server declaring `reset` as `std_srvs/srv/Trigger`:

```sh
ros2 service list -t
ros2 service call /reset std_srvs/srv/Trigger '{}'
```

Initialize every response field, including the bounded response string. A client
call can fail on refusal, timeout, or disconnect; handle that result in firmware.
Do not treat a transport acknowledgement as proof that a physical action
completed. The `examples/arduino_service` sketch serves `reset` and calls
`trigger` (both `std_srvs/srv/Trigger`). The topic-only compile helper does not
configure this build: it stages the bundled topic path, so compile the service
sketch with raw Arduino CLI instead, adding the service flag and a smaller
frame limit to the bundle's own defines:

```sh
arduino-cli compile --fqbn arduino:avr:uno \
  --build-property "compiler.c.extra_flags=-DR2S_WITH_SERVICES -DR2S_MAX_FRAME=64" \
  --build-property "compiler.cpp.extra_flags=-DR2S_WITH_SERVICES -DR2S_MAX_FRAME=64" \
  --library firmware/ros2serial --build-path firmware/build/service \
  --output-dir firmware/build/service examples/arduino_service
```

The staged library's `src/r2s_config.h` supplies the other limits, so only the
channel flag and the smaller frame are passed here. The frame stays at 64
because the service channel keeps three frame-sized buffers: larger frames
leave the Uno no room beside them.

## Actions

The MCU action API and the bridge action server exist: the bridge presents each
MCU action as a ROS 2 action server, and the end-to-end scenarios drive a
`tf2_msgs/action/LookupTransform` goal on posix (scenario 15) and avr
(scenario 16), covering a second goal while one runs, a cancel, and a
disconnect and a reset mid-goal.

Compile the action example with the actions flag and a frame of 128, as
`scripts/build-examples.sh` does:

```sh
arduino-cli compile --fqbn arduino:avr:uno \
  --build-property "compiler.c.extra_flags=-DR2S_WITH_ACTIONS -DR2S_MAX_FRAME=128" \
  --build-property "compiler.cpp.extra_flags=-DR2S_WITH_ACTIONS -DR2S_MAX_FRAME=128" \
  --library firmware/ros2serial --build-path firmware/build/action \
  --output-dir firmware/build/action examples/arduino_action
```

The `arduino_action` sketch serves the bundled `tf2_msgs/action/LookupTransform`
action: the goal, result and feedback structs, the announced type name and the
hash all come from the generated action header, so the sketch writes none of
them and a ROS client drives it. Serving your own action works the same way
once `r2s-gen` has emitted a library carrying it (see
[Custom interfaces](custom-interfaces.md)). The intended semantics and
resource limits are specified in the
[design](00-design.md#services-and-actions-p1).

### Starting an action from firmware

An action the MCU declares is an action it serves. The board never sends a
goal, and the API has no action client: the bridge is the ROS 2 action server
on the MCU's behalf, and the goal UUIDs, the cancel and the result are the
host's, per [ACT-2] and [SES-35] in
[the specification](../spec/protocol.md#79-actions-channel-id-8).
A firmware that has to start an action reaches it the other way round, through
a service: the sketch calls a small host node, that node sends the goal with
`rclcpp_action` or `rclpy`, and it answers the call when the goal is accepted
and finished.

The service is the sketch's own, so neither end needs a new type. This host
node serves `start_spin` as `std_srvs/srv/Trigger` and sends one `spin` goal,
the bundled `tf2_msgs/action/LookupTransform` that `examples/arduino_action`
serves, then reports the outcome in the response:

```python
import threading

import rclpy
from action_msgs.msg import GoalStatus
from rclpy.action import ActionClient
from rclpy.callback_groups import ReentrantCallbackGroup
from rclpy.executors import MultiThreadedExecutor
from rclpy.node import Node
from std_srvs.srv import Trigger
from tf2_msgs.action import LookupTransform


class Relay(Node):
    """Serves `start_spin` by sending one `spin` goal and returning its outcome."""

    def __init__(self):
        super().__init__('action_relay')
        # Reentrant: the service callback blocks until the goal's result, which
        # another callback delivers, so the executor needs two threads.
        group = ReentrantCallbackGroup()
        self._goals = ActionClient(self, LookupTransform, 'spin', callback_group=group)
        self._start = self.create_service(
            Trigger, 'start_spin', self._on_start, callback_group=group
        )

    def _on_start(self, request, response):
        # The MCU's Trigger response carries 16 characters, so every message
        # below fits inside the string the firmware decodes.
        if not self._goals.wait_for_server(timeout_sec=5.0):
            response.success = False
            response.message = 'no spin server'
            return response
        outcome = self._send_goal()
        response.success = outcome == GoalStatus.STATUS_SUCCEEDED
        response.message = _OUTCOME.get(outcome, 'timeout')
        return response

    def _send_goal(self):
        """Send the goal and return its terminal status, or None on a timeout."""
        finished = threading.Event()
        outcome = {}

        def on_result(future):
            outcome['status'] = future.result().status
            finished.set()

        def on_goal(future):
            handle = future.result()
            if not handle.accepted:
                finished.set()
                return
            handle.get_result_async().add_done_callback(on_result)

        self._goals.send_goal_async(LookupTransform.Goal()).add_done_callback(on_goal)
        if not finished.wait(timeout=30.0):
            return None
        return outcome.get('status')


_OUTCOME = {
    GoalStatus.STATUS_SUCCEEDED: 'succeeded',
    GoalStatus.STATUS_ABORTED: 'aborted',
    GoalStatus.STATUS_CANCELED: 'canceled',
}


def main():
    rclpy.init()
    node = Relay()
    executor = MultiThreadedExecutor(num_threads=2)
    executor.add_node(node)
    try:
        executor.spin()
    finally:
        executor.shutdown()
        node.destroy_node()
        rclpy.shutdown()


if __name__ == '__main__':
    main()
```

Run it beside the bridge, in a shell where the robot's workspace is sourced so
both nodes resolve their types. The sketch's side is an ordinary service
client, and it costs no new struct: on the MCU the action is whatever server
the sketch already runs.

```cpp
#define R2S_WITH_SERVICES 1

#include <ros2serial.h>
#include <r2s/service.hpp>
#include <std_srvs/srv/trigger.hpp>

r2s::Node node("arm");
r2s::Client<std_srvs::srv::Trigger> start_spin(node, "start_spin");

/* The call spins the node while it waits, so the servers the sketch runs
 * keep answering: a goal the relay sends back reaches them. */
static void request_grasp()
{
    std_srvs::srv::Trigger::Request req;
    std_srvs::srv::Trigger::Response res;

    req.structure_needs_at_least_one_member = 0u;
    if (!start_spin.call(req, res)) {
        return;
    }
    /* res.success reports the goal's outcome, not the transport's. */
    (void)res.success;
}
```

Three things follow from the route. The call blocks until the goal finishes,
so a call the answer never reaches costs the firmware
`R2S_SVC_CALL_TIMEOUT_MS`, 5 s unless the build sets another ([SVR-8]) and the
goal it started keeps running. The response carries 16 characters in the
bundled `Trigger`, so a relay that reports a longer message fails the
firmware's decode instead of truncating it. And a goal the host starts stays
the host's: a cancel from the MCU has to travel the same way, as its own call
and its own node.

The relay is the host's own program, so nothing here changes the wire: the goal
and the result stay in the host, and only the service call and its answer cross
the link.
