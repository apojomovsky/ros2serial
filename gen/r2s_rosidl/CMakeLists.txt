# The colcon-time wrapper over r2s-gen: install the CMake helper a firmware
# package finds to generate its sources during the build.
cmake_minimum_required(VERSION 3.22)
project(r2s_rosidl)

find_package(ament_cmake REQUIRED)

install(DIRECTORY cmake/ DESTINATION share/${PROJECT_NAME}/cmake)

ament_package(CONFIG_EXTRAS cmake/r2s_rosidl-extras.cmake)
