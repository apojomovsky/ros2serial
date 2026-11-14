# Parameters, services, and actions

Start with [topics](firmware.md). Optional channels add firmware storage and
build settings; their availability depends on the API and configuration you
compile. The basic Arduino helper prepares the bundled publish/subscribe path.
It does not automatically enable services, parameters, or actions.

## Parameters

Firmware declares a parameter table before its first spin. With
`R2S_WITH_PARAMS` enabled in all firmware translation units, those declarations
become parameters on the bridge node; the sets travel over the reliable channel.
They are not parameters of a separate ROS node named after the MCU.

The bundled Arduino helper leaves the parameter channel out, so add the flag
the way the services recipe below adds its own: `-DR2S_WITH_PARAMS` in both
`compiler.c.extra_flags` and `compiler.cpp.extra_flags`.

The [parameter example](03-port-rosserial-sketch.md#parameters) shows a boolean
`led` and a bounded string `label`, including the storage the MCU owns.
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
client calling a ROS server. The implemented sketch interface uses the rosserial
compatibility API. See the complete
[service declarations and callbacks](03-port-rosserial-sketch.md#services).

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
  --build-property "compiler.c.extra_flags=$extras -DR2S_WITH_SERVICES -DR2S_MAX_FRAME=64" \
  --build-property "compiler.cpp.extra_flags=$extras -DR2S_WITH_SERVICES -DR2S_MAX_FRAME=64" \
  --library firmware/ros2serial --build-path firmware/build/service \
  --output-dir firmware/build/service examples/arduino_service
```

`$extras` is the bundle's required defines (`R2S_WITH_CODEC` with its limits);
`scripts/build-examples.sh` derives them from the staged `r2s_bundled.h` and is
the reference for the exact set. The frame stays at 64 because the service
channel keeps three frame-sized buffers: larger frames leave the Uno no room
beside them.

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
  --build-property "compiler.c.extra_flags=$extras -DR2S_WITH_ACTIONS -DR2S_MAX_FRAME=128" \
  --build-property "compiler.cpp.extra_flags=$extras -DR2S_WITH_ACTIONS -DR2S_MAX_FRAME=128" \
  --library firmware/ros2serial --build-path firmware/build/action \
  --output-dir firmware/build/action examples/arduino_action
```

The `arduino_action` sketch announces a placeholder type name and hash over
stand-in Float32 payloads, so it proves the MCU wiring over raw frames and no
ROS client can drive it. A ROS goal needs firmware announcing an installed
action type. The intended semantics and resource limits are specified in the
[design](00-design.md#services-and-actions-p1).
