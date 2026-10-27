# Integration tests for the bridge launch file: launch_testing cases that drive
# the installed r2s_bridge launch file against the posix virtual MCU.
cmake_minimum_required(VERSION 3.16)
project(r2s_bridge_tests)

find_package(ament_cmake REQUIRED)

if(BUILD_TESTING)
  find_package(launch_testing_ament_cmake REQUIRED)
  add_launch_test(test/test_bridge_posix_launch.py TIMEOUT 300)
endif()

ament_package()
