# ros2serial

rosserial for ROS 2: publish and subscribe to ROS 2 topics from small microcontrollers, down to an 8-bit Arduino Uno, over a serial link.

**Status:** design and bootstrap. Nothing is usable yet. The design is in [`docs/00-design.md`](docs/00-design.md), and the work in the [issues](https://github.com/apojomovsky/ros2serial/issues).

## What it will look like

```c
#define R2S_NODE_NAME "base_controller"
#define R2S_TOPICS(X) \
  X(PUB, battery, std_msgs, Float32) \
  X(SUB, cmd_vel, geometry_msgs, Twist)
#include <ros2serial.h>
```

Install the Arduino library, declare your topics, publish, call spin. On the host, a regular ROS 2 node bridges the serial port to any RMW.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md).

## License

MIT, see [`LICENSE`](LICENSE).
