# Parameters, services, and actions

Start with [topics](firmware.md). Optional channels add firmware storage and
build settings; their availability depends on the API and configuration you
compile. The basic Arduino helper prepares the bundled publish/subscribe path.
It does not automatically enable services, parameters, or actions.

## Parameters

Firmware declares a parameter table before its first spin. With
`R2S_WITH_PARAMS` and the reliable channel enabled in all firmware translation
units, those declarations become parameters on the bridge node. They are not
parameters of a separate ROS node named after the MCU.

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

Enable `R2S_WITH_SERVICES` and `R2S_WITH_RELIABLE` for the core and runtime as
well as the sketch. For an MCU server declaring `reset` as `std_srvs/srv/Trigger`:

```sh
ros2 service list -t
ros2 service call /reset std_srvs/srv/Trigger '{}'
```

Initialize every response field, including the bounded response string. A client
call can fail on refusal, timeout, or disconnect; handle that result in firmware.
Do not treat a transport acknowledgement as proof that a physical action
completed. A ready-to-run Arduino service example is still tracked in
[#561](https://github.com/apojomovsky/ros2serial/issues/561); the basic topic
compile helper does not yet configure this advanced build.

## Actions

The generator and MCU action API exist, and `examples/arduino_action` demonstrates
the firmware side. The host bridge action server and its end-to-end validation
remain tracked in [#319](https://github.com/apojomovsky/ros2serial/issues/319)
and [#320](https://github.com/apojomovsky/ros2serial/issues/320).

Do not expect `ros2 action send_goal` to reach that sketch through the current
bridge. Once the host integration lands, it should provide the normal ROS action
interface. The intended semantics and resource limits are specified in the
[design](00-design.md#services-and-actions-p1).
