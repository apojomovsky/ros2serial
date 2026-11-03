# Bundled message snapshots

r2s-gen resolves the bundled types without a ROS installation from these
definitions. Each distro directory keeps the upstream package layout,
`<package>/msg/<Type>.msg`, and the search path reads it like a message root.

The files are copied from the ROS 2 common_interfaces,
rcl_interfaces and geometry2 repositories at the commits below. The first two
are under the Apache 2.0 license
([common_interfaces LICENSE](https://github.com/ros2/common_interfaces/blob/jazzy/LICENSE),
[rcl_interfaces LICENSE](https://github.com/ros2/rcl_interfaces/blob/jazzy/LICENSE));
geometry2 is under the BSD 3-Clause license
([geometry2 LICENSE](https://github.com/ros2/geometry2/blob/jazzy/LICENSE)).
rcl_interfaces supplies `builtin_interfaces/Time`, which `std_msgs/Header`
contains; geometry2 supplies `tf2_msgs/TFMessage`, which carries the
`geometry_msgs/TransformStamped` array the tf broadcaster publishes.

| Distro | common_interfaces commit | rcl_interfaces commit | geometry2 commit |
| --- | --- | --- | --- |
| jazzy | `a941f14bb318d8d904505ed935ccbb97f24a70a4` | `7aa3caf43377ea6ad615bc1040832e2c7566bfbe` | `f702874b1c8535d6a038230ab2cda0ba5d521ebd` |
| lyrical | `ea081d8a282f08f4df5f93527c3248674aed954b` | `b90e36e4adf5ed878efa2f365a84798f782618cd` | `0b06614aaf3a50ddd1298cb0d95224b0e3821b25` |
| rolling | `803f3d532cbfbabd4fdf1b35a8bcd9aa6b27585f` | `56800da135660030bb816c72794afba15fe3a538` | `6554db56521f3dfc6647797e832b109abbac722b` |

To add a type, copy it from the same repositories at the commit its distro row
names. A distro's directory holds only the types the generated code and the
bundled Arduino library use.
