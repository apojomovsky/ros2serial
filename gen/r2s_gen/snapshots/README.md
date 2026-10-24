# Bundled message snapshots

r2s-gen resolves the bundled types without a ROS installation from these
definitions. Each distro directory keeps the upstream package layout,
`<package>/msg/<Type>.msg`, and the search path reads it like a message root.

The files are copied from the ROS 2 common_interfaces and
rcl_interfaces repositories at the commits below, under the Apache 2.0 license
([common_interfaces LICENSE](https://github.com/ros2/common_interfaces/blob/jazzy/LICENSE),
[rcl_interfaces LICENSE](https://github.com/ros2/rcl_interfaces/blob/jazzy/LICENSE)).
rcl_interfaces supplies `builtin_interfaces/Time`, which `std_msgs/Header`
contains.

| Distro | common_interfaces commit | rcl_interfaces commit |
| --- | --- | --- |
| jazzy | `a941f14bb318d8d904505ed935ccbb97f24a70a4` | `7aa3caf43377ea6ad615bc1040832e2c7566bfbe` |
| lyrical | `ea081d8a282f08f4df5f93527c3248674aed954b` | `b90e36e4adf5ed878efa2f365a84798f782618cd` |
| rolling | `803f3d532cbfbabd4fdf1b35a8bcd9aa6b27585f` | `56800da135660030bb816c72794afba15fe3a538` |

To add a type, copy it from the same repositories at the commit its distro row
names. A distro's directory holds only the types the generated code and the
bundled Arduino library use.
