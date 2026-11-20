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
