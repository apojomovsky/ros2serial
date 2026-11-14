# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Alexis Pojomovsky and ros2serial contributors

# The example interface package: one message, one service and one action, so
# the custom-type walkthrough and the firmware example share one definition.
cmake_minimum_required(VERSION 3.22)
project(custom_interfaces)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/SensorReading.msg"
  "srv/Calibrate.srv"
  "action/Run.action")

ament_export_dependencies(rosidl_default_runtime)
ament_package()
